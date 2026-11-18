#define _POSIX_C_SOURCE 200809L
#include "http_client.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HTTP_CONNECT_TIMEOUT_ARG "15"
#define HTTP_MAX_TIME_ARG "30"
#define HTTP_USER_AGENT "NextUI-RA-Cache/0.1"
#define HTTP_MAX_RETRIES 4
#define HTTP_INITIAL_BACKOFF_MS 2000
#define HTTP_READ_CHUNK 4096
#define HTTP_MAX_RESPONSE_BYTES (16u * 1024u * 1024u)
#define HTTP_STATUS_TOO_MANY_REQUESTS 429
#define HTTP_STATUS_OK 200
#define MS_PER_SEC 1000LL
#define NS_PER_MS 1000000LL

static int64_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * MS_PER_SEC + (int64_t)(ts.tv_nsec / NS_PER_MS);
}

static void sleep_ms(int ms) {
	if (ms <= 0) {
		return;
	}
	struct timespec req = {
		.tv_sec = ms / MS_PER_SEC,
		.tv_nsec = (long)(ms % MS_PER_SEC) * NS_PER_MS,
	};
	while (nanosleep(&req, &req) == -1 && errno == EINTR) {
	}
}

static void enforce_rate_limit(HttpClient* client) {
	if (client->delay_ms <= 0 || client->last_request_ms == 0) {
		return;
	}
	int64_t elapsed = monotonic_ms() - client->last_request_ms;
	if (elapsed < client->delay_ms) {
		sleep_ms((int)(client->delay_ms - elapsed));
	}
}

static bool write_all(int fd, const char* data, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		off += (size_t)n;
	}
	return true;
}

static bool read_all_fd(int fd, char** out_buf, size_t* out_len) {
	size_t cap = HTTP_READ_CHUNK;
	size_t len = 0;
	char* buf = malloc(cap);
	if (!buf) {
		return false;
	}

	for (;;) {
		if (len + HTTP_READ_CHUNK + 1 > cap) {
			if (cap >= HTTP_MAX_RESPONSE_BYTES) {
				free(buf);
				return false;
			}
			size_t new_cap = cap * 2;
			if (new_cap > HTTP_MAX_RESPONSE_BYTES + 1) {
				new_cap = HTTP_MAX_RESPONSE_BYTES + 1;
			}
			char* grown = realloc(buf, new_cap);
			if (!grown) {
				free(buf);
				return false;
			}
			buf = grown;
			cap = new_cap;
		}

		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			free(buf);
			return false;
		}
		if (n == 0) {
			break;
		}
		len += (size_t)n;
	}

	buf[len] = '\0';
	*out_buf = buf;
	*out_len = len;
	return true;
}

static bool exec_curl(char* const argv[], const char* stdin_data, char** out_stdout, size_t* out_len) {
	int stdin_pipe[2] = { -1, -1 };
	int stdout_pipe[2] = { -1, -1 };

	if (stdin_data && pipe(stdin_pipe) != 0) {
		return false;
	}
	if (pipe(stdout_pipe) != 0) {
		if (stdin_data) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
		}
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		if (stdin_data) {
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
		}
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		return false;
	}

	if (pid == 0) {
		if (stdin_data) {
			dup2(stdin_pipe[0], STDIN_FILENO);
			close(stdin_pipe[0]);
			close(stdin_pipe[1]);
		}
		dup2(stdout_pipe[1], STDOUT_FILENO);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		execvp("curl", argv);
		_exit(127);
	}

	if (stdin_data) {
		close(stdin_pipe[0]);
		size_t slen = strlen(stdin_data);
		(void)write_all(stdin_pipe[1], stdin_data, slen);
		close(stdin_pipe[1]);
	}
	close(stdout_pipe[1]);

	bool read_ok = read_all_fd(stdout_pipe[0], out_stdout, out_len);
	close(stdout_pipe[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
	}

	if (!read_ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		free(*out_stdout);
		*out_stdout = NULL;
		*out_len = 0;
		return false;
	}
	return true;
}

void http_client_init(HttpClient* client, int delay_ms) {
	signal(SIGPIPE, SIG_IGN);
	client->delay_ms = delay_ms >= 0 ? delay_ms : HTTP_DEFAULT_DELAY_MS;
	client->last_request_ms = 0;
}

void http_response_free(HttpResponse* resp) {
	if (!resp) {
		return;
	}
	free(resp->body);
	resp->body = NULL;
	resp->body_len = 0;
	resp->status_code = 0;
}

static bool post_once(HttpClient* client, const char* url, const char* post_data, HttpResponse* out_resp) {
	enforce_rate_limit(client);

	char* argv[] = {
		"curl",
		"--silent",
		"--show-error",
		"--connect-timeout", HTTP_CONNECT_TIMEOUT_ARG,
		"--max-time", HTTP_MAX_TIME_ARG,
		"-A", HTTP_USER_AGENT,
		"-H", "Content-Type: application/x-www-form-urlencoded",
		"--data-binary", "@-",
		"--write-out", "\n%{http_code}",
		(char*)url,
		NULL
	};

	char* raw = NULL;
	size_t raw_len = 0;
	bool ok = exec_curl(argv, post_data ? post_data : "", &raw, &raw_len);
	client->last_request_ms = monotonic_ms();
	if (!ok || !raw) {
		return false;
	}

	char* last_nl = strrchr(raw, '\n');
	if (!last_nl) {
		free(raw);
		return false;
	}

	*last_nl = '\0';
	int code = (int)strtol(last_nl + 1, NULL, 10);
	out_resp->status_code = code;
	out_resp->body = raw;
	out_resp->body_len = (size_t)(last_nl - raw);
	return code > 0;
}

bool http_post_form(HttpClient* client, const char* url, const char* post_data, HttpResponse* out_resp) {
	memset(out_resp, 0, sizeof(*out_resp));
	int backoff_ms = HTTP_INITIAL_BACKOFF_MS;

	for (int attempt = 0; attempt <= HTTP_MAX_RETRIES; attempt++) {
		if (!post_once(client, url, post_data, out_resp)) {
			return false;
		}
		if (out_resp->status_code != HTTP_STATUS_TOO_MANY_REQUESTS) {
			return true;
		}
		if (attempt < HTTP_MAX_RETRIES) {
			fprintf(stderr, "HTTP 429 rate limited, backing off %d ms...\n", backoff_ms);
			http_response_free(out_resp);
			sleep_ms(backoff_ms);
			backoff_ms *= 2;
		}
	}
	return true;
}

static int download_once(HttpClient* client, const char* url, const char* tmp_path) {
	enforce_rate_limit(client);

	char* argv[] = {
		"curl",
		"--silent",
		"--show-error",
		"--location",
		"--connect-timeout", HTTP_CONNECT_TIMEOUT_ARG,
		"--max-time", HTTP_MAX_TIME_ARG,
		"-A", HTTP_USER_AGENT,
		"--write-out", "%{http_code}",
		"-o", (char*)tmp_path,
		(char*)url,
		NULL
	};

	char* out = NULL;
	size_t out_len = 0;
	bool ok = exec_curl(argv, NULL, &out, &out_len);
	client->last_request_ms = monotonic_ms();
	if (!ok || !out) {
		unlink(tmp_path);
		return -1;
	}

	int code = (int)strtol(out, NULL, 10);
	free(out);
	return code > 0 ? code : -1;
}

int http_download_file(HttpClient* client, const char* url, const char* dest_path) {
	char tmp_path[1024];
	int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dest_path);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		return -1;
	}

	int backoff_ms = HTTP_INITIAL_BACKOFF_MS;
	int code = -1;

	for (int attempt = 0; attempt <= HTTP_MAX_RETRIES; attempt++) {
		code = download_once(client, url, tmp_path);
		if (code != HTTP_STATUS_TOO_MANY_REQUESTS) {
			break;
		}
		unlink(tmp_path);
		if (attempt < HTTP_MAX_RETRIES) {
			fprintf(stderr, "HTTP 429 rate limited on download, backing off %d ms...\n", backoff_ms);
			sleep_ms(backoff_ms);
			backoff_ms *= 2;
		}
	}

	if (code == HTTP_STATUS_OK) {
		struct stat st;
		if (stat(tmp_path, &st) == 0 && st.st_size > 0 && rename(tmp_path, dest_path) == 0) {
			return code;
		}
	}

	unlink(tmp_path);
	return code;
}
