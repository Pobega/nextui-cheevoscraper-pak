package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	gaba "github.com/BrandonKowalski/gabagool/v2/pkg/gabagool"
	"github.com/BrandonKowalski/gabagool/v2/pkg/gabagool/constants"
	uatomic "go.uber.org/atomic"
)

type AppConfig struct {
	Badges   string
	SkipDisc bool
}

type SystemEntry struct {
	Folder string
	Tag    string
	Count  int
}

type GameJSONItem struct {
	Name     string   `json:"name"`
	Options  []string `json:"options"`
	Selected int      `json:"selected"`
}

type GamesJSONPayload struct {
	Games []GameJSONItem `json:"games"`
}

func settingsFilePath() string {
	userdata := os.Getenv("USERDATA_PATH")
	if userdata == "" {
		userdata = "/tmp"
	}
	return filepath.Join(userdata, "CheevoScraper", "settings.conf")
}

func loadSettings() AppConfig {
	cfg := AppConfig{Badges: "all", SkipDisc: false}
	data, err := os.ReadFile(settingsFilePath())
	if err != nil {
		return cfg
	}
	for _, line := range strings.Split(string(data), "\n") {
		parts := strings.SplitN(strings.TrimSpace(line), "=", 2)
		if len(parts) != 2 {
			continue
		}
		switch parts[0] {
		case "BADGES":
			if parts[1] == "all" || parts[1] == "unlocked" || parts[1] == "none" {
				cfg.Badges = parts[1]
			}
		case "SKIP_DISC":
			cfg.SkipDisc = parts[1] == "1"
		}
	}
	return cfg
}

func saveSettings(cfg AppConfig) {
	p := settingsFilePath()
	_ = os.MkdirAll(filepath.Dir(p), 0o755)
	skip := "0"
	if cfg.SkipDisc {
		skip = "1"
	}
	content := fmt.Sprintf("BADGES=%s\nSKIP_DISC=%s\n", cfg.Badges, skip)
	_ = os.WriteFile(p, []byte(content), 0o644)
}

func findRaCacheBin() string {
	if exe, err := os.Executable(); err == nil {
		candidate := filepath.Join(filepath.Dir(exe), "ra-cache")
		if _, statErr := os.Stat(candidate); statErr == nil {
			return candidate
		}
	}
	return "ra-cache"
}

func runRaCacheCapture(bin string, args ...string) (string, int, error) {
	cmd := exec.Command(bin, args...)
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &out
	err := cmd.Run()
	exitCode := 0
	if err != nil {
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) {
			exitCode = exitErr.ExitCode()
		} else {
			exitCode = 1
		}
	}
	return strings.TrimSpace(out.String()), exitCode, err
}

func showMessage(msg string, confirmText string) {
	_, _ = gaba.ConfirmationMessage(msg, []gaba.FooterHelpItem{
		{ButtonName: "A", HelpText: confirmText},
	}, gaba.MessageOptions{})
}

func confirmAction(msg string) bool {
	res, err := gaba.ConfirmationMessage(msg, []gaba.FooterHelpItem{
		{ButtonName: "A", HelpText: "Continue"},
		{ButtonName: "B", HelpText: "Cancel"},
	}, gaba.MessageOptions{})
	if err != nil || gaba.IsCancelled(err) || res == nil {
		return false
	}
	return res.Confirmed
}

func runSyncWithProgress(bin string, cfg AppConfig, title string, extraArgs ...string) bool {
	args := []string{"--badges", cfg.Badges}
	if cfg.SkipDisc {
		args = append(args, "--skip-disc-systems")
	}
	args = append(args, extraArgs...)

	fifoPath := filepath.Join(os.TempDir(), fmt.Sprintf("cheevoscraper-%d.fifo", os.Getpid()))
	_ = os.Remove(fifoPath)
	if err := syscall.Mkfifo(fifoPath, 0o600); err == nil {
		args = append(args, "--fifo", fifoPath)
		defer os.Remove(fifoPath)
	} else {
		fifoPath = ""
	}

	progress := uatomic.NewFloat64(0)
	cmd := exec.Command(bin, args...)
	var outBuf bytes.Buffer
	cmd.Stdout = &outBuf
	cmd.Stderr = &outBuf

	summary, err := gaba.ProcessMessage(title, gaba.ProcessMessageOptions{
		ShowThemeBackground: true,
		ShowProgressBar:     true,
		Progress:            progress,
		ProcessInput:        true,
		CancelButton:        constants.VirtualButtonB,
		FooterHelpItems: []gaba.FooterHelpItem{
			{ButtonName: "B", HelpText: "Cancel"},
		},
	}, func() (string, error) {
		if startErr := cmd.Start(); startErr != nil {
			return "", startErr
		}

		doneReader := make(chan struct{})
		if fifoPath != "" {
			go func() {
				defer close(doneReader)
				f, openErr := os.Open(fifoPath)
				if openErr != nil {
					return
				}
				defer f.Close()
				scanner := bufio.NewScanner(f)
				for scanner.Scan() {
					line := scanner.Text()
					if strings.HasPrefix(line, "PROGRESS:") {
						if pct, convErr := strconv.Atoi(strings.TrimPrefix(line, "PROGRESS:")); convErr == nil {
							if pct < 0 {
								pct = 0
							} else if pct > 100 {
								pct = 100
							}
							progress.Store(float64(pct) / 100.0)
						}
					}
				}
			}()
		} else {
			close(doneReader)
		}

		waitErr := cmd.Wait()
		<-doneReader
		progress.Store(1.0)

		output := outBuf.String()
		for _, line := range strings.Split(output, "\n") {
			if strings.HasPrefix(line, "SUMMARY:") {
				return strings.TrimSpace(strings.TrimPrefix(line, "SUMMARY:")), nil
			}
		}
		if waitErr != nil {
			return "", fmt.Errorf("sync failed: %v", waitErr)
		}
		return "Sync complete", nil
	})

	if gaba.IsCancelled(err) {
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
		showMessage("Sync cancelled.", "OK")
		return true
	}
	if err != nil {
		showMessage(fmt.Sprintf("Error: %v", err), "OK")
		return false
	}

	showMessage(summary, "OK")
	return true
}

func loadSystems(bin string, cfg AppConfig) ([]SystemEntry, error) {
	return gaba.ProcessMessage("Scanning ROM systems...", gaba.ProcessMessageOptions{
		ShowThemeBackground: true,
	}, func() ([]SystemEntry, error) {
		args := []string{"--list-systems"}
		if cfg.SkipDisc {
			args = append(args, "--skip-disc-systems")
		}
		out, rc, err := runRaCacheCapture(bin, args...)
		if err != nil || rc != 0 {
			return nil, fmt.Errorf("failed to scan ROM systems (exit %d)", rc)
		}
		var systems []SystemEntry
		for _, line := range strings.Split(out, "\n") {
			line = strings.TrimSpace(line)
			if line == "" {
				continue
			}
			parts := strings.Split(line, "\t")
			if len(parts) < 3 {
				continue
			}
			cnt, _ := strconv.Atoi(parts[2])
			systems = append(systems, SystemEntry{
				Folder: parts[0],
				Tag:    parts[1],
				Count:  cnt,
			})
		}
		return systems, nil
	})
}

func selectGamesChecklist(bin string, cfg AppConfig, sys SystemEntry) bool {
	payload, err := gaba.ProcessMessage(fmt.Sprintf("Scanning %s...", sys.Folder), gaba.ProcessMessageOptions{
		ShowThemeBackground: true,
	}, func() (GamesJSONPayload, error) {
		out, rc, runErr := runRaCacheCapture(bin, "--list-games-json", sys.Folder)
		if runErr != nil || rc != 0 {
			return GamesJSONPayload{}, fmt.Errorf("failed to list games (exit %d)", rc)
		}
		var parsed GamesJSONPayload
		if jsonErr := json.Unmarshal([]byte(out), &parsed); jsonErr != nil {
			return GamesJSONPayload{}, jsonErr
		}
		return parsed, nil
	})
	if err != nil {
		showMessage(fmt.Sprintf("Error: %v", err), "OK")
		return false
	}
	if len(payload.Games) == 0 {
		showMessage("No ROMs found in "+sys.Folder, "OK")
		return false
	}

	items := make([]gaba.MenuItem, len(payload.Games))
	for i, g := range payload.Games {
		label := g.Name
		if len(g.Options) > 0 && g.Options[0] == "Cached" {
			label = "[Cached] " + label
		}
		items[i] = gaba.MenuItem{
			Text:     label,
			Metadata: i,
		}
	}

	opts := gaba.DefaultListOptions(sys.Folder, items)
	opts.InitialMultiSelectMode = true
	opts.SelectAllButton = constants.VirtualButtonX
	opts.DeselectAllButton = constants.VirtualButtonY
	opts.MultiSelectConfirmButton = constants.VirtualButtonStart
	opts.FooterHelpItems = []gaba.FooterHelpItem{
		{ButtonName: "A", HelpText: "Toggle"},
		{ButtonName: "X", HelpText: "All"},
		{ButtonName: "Y", HelpText: "None"},
		{ButtonName: "START", HelpText: "Sync", IsConfirmButton: true},
		{ButtonName: "B", HelpText: "Back"},
	}

	res, listErr := gaba.List(opts)
	if listErr != nil || gaba.IsCancelled(listErr) || res == nil || len(res.Selected) == 0 {
		return false
	}

	selectedSet := make(map[int]bool, len(res.Selected))
	for _, idx := range res.Selected {
		selectedSet[idx] = true
	}
	for i := range payload.Games {
		if selectedSet[i] {
			payload.Games[i].Selected = 1
		} else {
			payload.Games[i].Selected = 0
		}
	}

	tmpFile := filepath.Join(os.TempDir(), fmt.Sprintf("cheevoscraper-games-%d.json", os.Getpid()))
	data, err := json.Marshal(payload)
	if err != nil {
		return false
	}
	if writeErr := os.WriteFile(tmpFile, data, 0o600); writeErr != nil {
		return false
	}
	defer os.Remove(tmpFile)

	return runSyncWithProgress(bin, cfg, "Syncing selected games...",
		"--system", sys.Folder, "--sync-json", tmpFile, "--force")
}

func systemMenu(bin string, cfg AppConfig) bool {
	systems, err := loadSystems(bin, cfg)
	if err != nil {
		showMessage(fmt.Sprintf("Error: %v", err), "OK")
		return false
	}
	if len(systems) == 0 {
		showMessage("No supported ROM systems found.", "OK")
		return false
	}

	sysItems := make([]gaba.MenuItem, len(systems))
	for i, s := range systems {
		sysItems[i] = gaba.MenuItem{
			Text:     fmt.Sprintf("%s (%d ROMs)", s.Folder, s.Count),
			Metadata: s,
		}
	}

	changed := false
	selIdx := 0
	for {
		opts := gaba.DefaultListOptions("Sync by System", sysItems)
		opts.SelectedIndex = selIdx
		opts.FooterHelpItems = []gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "Select"},
			{ButtonName: "B", HelpText: "Back"},
		}
		res, listErr := gaba.List(opts)
		if listErr != nil || gaba.IsCancelled(listErr) || res == nil || len(res.Selected) == 0 {
			return changed
		}
		selIdx = res.Selected[0]
		sys := systems[selIdx]

		for {
			actItems := []gaba.MenuItem{
				{Text: "Sync Uncached in " + sys.Tag},
				{Text: "Sync All in " + sys.Tag},
				{Text: "Select Games Manually..."},
				{Text: "Back"},
			}
			actOpts := gaba.DefaultListOptions(sys.Folder, actItems)
			actOpts.FooterHelpItems = []gaba.FooterHelpItem{
				{ButtonName: "A", HelpText: "Select"},
				{ButtonName: "B", HelpText: "Back"},
			}
			actRes, actErr := gaba.List(actOpts)
			if actErr != nil || gaba.IsCancelled(actErr) || actRes == nil || len(actRes.Selected) == 0 {
				break
			}
			switch actRes.Selected[0] {
			case 0:
				if runSyncWithProgress(bin, cfg, "Syncing "+sys.Tag+"...", "--system", sys.Folder) {
					changed = true
				}
			case 1:
				if confirmAction("Re-hash and re-fetch all games in " + sys.Tag + "?") {
					if runSyncWithProgress(bin, cfg, "Rebuilding "+sys.Tag+"...", "--system", sys.Folder, "--force") {
						changed = true
					}
				}
			case 2:
				if selectGamesChecklist(bin, cfg, sys) {
					changed = true
				}
			default:
				goto nextSystemLoop
			}
		}
	nextSystemLoop:
	}
}

func cacheToolsMenu(bin string, cfg AppConfig) bool {
	changed := false
	for {
		opts := gaba.DefaultListOptions("Cache Tools", []gaba.MenuItem{
			{Text: "Force Rebuild Cache"},
			{Text: "Re-check Unrecognized"},
			{Text: "Cache Info"},
			{Text: "Back"},
		})
		opts.FooterHelpItems = []gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "Select"},
			{ButtonName: "B", HelpText: "Back"},
		}
		res, err := gaba.List(opts)
		if err != nil || gaba.IsCancelled(err) || res == nil || len(res.Selected) == 0 {
			return changed
		}
		switch res.Selected[0] {
		case 0:
			if confirmAction("Re-hash and re-fetch ALL games?\nThis may take a long time.") {
				if runSyncWithProgress(bin, cfg, "Rebuilding cache...", "--force") {
					changed = true
				}
			}
		case 1:
			out, _ := gaba.ProcessMessage("Clearing unrecognized entries...", gaba.ProcessMessageOptions{
				ShowThemeBackground: true,
			}, func() (string, error) {
				s, _, _ := runRaCacheCapture(bin, "--clear-unrecognized")
				return s, nil
			})
			changed = true
			if out == "" {
				out = "Done."
			}
			showMessage(out, "OK")
		case 2:
			info, _ := gaba.ProcessMessage("Calculating cache stats...", gaba.ProcessMessageOptions{
				ShowThemeBackground: true,
			}, func() (string, error) {
				s, _, _ := runRaCacheCapture(bin, "--cache-info")
				return s, nil
			})
			showMessage(info, "OK")
		default:
			return changed
		}
	}
}

func settingsMenu(cfg AppConfig) AppConfig {
	badgeIdx := 0
	switch cfg.Badges {
	case "unlocked":
		badgeIdx = 1
	case "none":
		badgeIdx = 2
	}
	discIdx := 0
	if cfg.SkipDisc {
		discIdx = 1
	}

	items := []gaba.ItemWithOptions{
		{
			Item: gaba.MenuItem{Text: "Download Badges"},
			Options: []gaba.Option{
				{DisplayName: "All", Value: "all"},
				{DisplayName: "Unlocked Only", Value: "unlocked"},
				{DisplayName: "None", Value: "none"},
			},
			SelectedOption: badgeIdx,
		},
		{
			Item: gaba.MenuItem{Text: "Skip Disc Systems"},
			Options: []gaba.Option{
				{DisplayName: "Off", Value: false},
				{DisplayName: "On", Value: true},
			},
			SelectedOption: discIdx,
		},
	}

	res, err := gaba.OptionsList("CheevoScraper Settings", gaba.OptionListSettings{
		FooterHelpItems: []gaba.FooterHelpItem{
			{ButtonName: "LEFT/RIGHT", HelpText: "Change"},
			{ButtonName: "START", HelpText: "Save", IsConfirmButton: true},
			{ButtonName: "B", HelpText: "Cancel"},
		},
	}, items)
	if err != nil || gaba.IsCancelled(err) || res == nil {
		return cfg
	}

	if v, ok := res.Items[0].Options[res.Items[0].SelectedOption].Value.(string); ok {
		cfg.Badges = v
	}
	if v, ok := res.Items[1].Options[res.Items[1].SelectedOption].Value.(bool); ok {
		cfg.SkipDisc = v
	}
	saveSettings(cfg)
	return cfg
}

func main() {
	gaba.Init(gaba.Options{
		WindowTitle:    "CheevoScraper",
		ShowBackground: true,
		IsNextUI:       true,
	})
	defer gaba.Close()

	bin := findRaCacheBin()
	statusLine, rc, _ := runRaCacheCapture(bin, "--status")
	if rc == 2 {
		if statusLine == "" {
			statusLine = "Not logged in (configure RA in NextUI Settings)"
		}
		showMessage(statusLine, "OK")
		return
	}
	if rc != 0 {
		showMessage(fmt.Sprintf("Failed to read RA status (exit %d)", rc), "OK")
		return
	}

	cfg := loadSettings()
	selIdx := 0

	for {
		opts := gaba.DefaultListOptions(statusLine, []gaba.MenuItem{
			{Text: "Sync by System..."},
			{Text: "Sync New Games"},
			{Text: "Refresh Sessions"},
			{Text: "Cache Tools..."},
			{Text: "Settings"},
			{Text: "Exit"},
		})
		opts.SelectedIndex = selIdx
		opts.FooterHelpItems = []gaba.FooterHelpItem{
			{ButtonName: "A", HelpText: "Select"},
			{ButtonName: "B", HelpText: "Exit"},
		}

		res, err := gaba.List(opts)
		if err != nil || gaba.IsCancelled(err) || res == nil || len(res.Selected) == 0 {
			return
		}
		selIdx = res.Selected[0]

		refreshStatus := false
		switch selIdx {
		case 0:
			refreshStatus = systemMenu(bin, cfg)
		case 1:
			if confirmAction("Scan all ROM folders and cache new games?\nThis may take a while.") {
				refreshStatus = runSyncWithProgress(bin, cfg, "Syncing new games...")
			}
		case 2:
			if confirmAction("Refresh session unlocks for all cached games?") {
				refreshStatus = runSyncWithProgress(bin, cfg, "Refreshing sessions...", "--refresh-sessions")
			}
		case 3:
			refreshStatus = cacheToolsMenu(bin, cfg)
		case 4:
			cfg = settingsMenu(cfg)
		default:
			return
		}

		if refreshStatus {
			if nextStatus, nextRC, _ := runRaCacheCapture(bin, "--status"); nextRC == 0 && nextStatus != "" {
				statusLine = nextStatus
			}
		}
	}
}
