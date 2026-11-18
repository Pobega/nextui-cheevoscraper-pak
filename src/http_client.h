#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTP_DEFAULT_DELAY_MS 250

typedef struct {
	int delay_ms;
	int64_t last_request_ms;
} HttpClient;

typedef struct {
	int status_code;
	char* body;
	size_t body_len;
} HttpResponse;

void http_client_init(HttpClient* client, int delay_ms);
void http_response_free(HttpResponse* resp);

/**
 * Perform an HTTP POST with application/x-www-form-urlencoded body via curl.
 * Enforces rate-limit delay and exponential backoff on HTTP 429.
 * Returns true if curl executed and a valid HTTP status was parsed.
 */
bool http_post_form(HttpClient* client, const char* url, const char* post_data, HttpResponse* out_resp);

/**
 * Download a URL directly to dest_path atomically via curl.
 * Enforces rate-limit delay and exponential backoff on HTTP 429.
 * Returns HTTP status code (200 on success), or -1 on local/exec error.
 */
int http_download_file(HttpClient* client, const char* url, const char* dest_path);

#endif
