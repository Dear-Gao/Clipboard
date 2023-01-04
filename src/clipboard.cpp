/*  Clipboard - Cut, copy, and paste anything, anywhere, all from the terminal.
    Copyright (C) 2023 Jackson Huff and other contributors on GitHub.com
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.*/
#include <vector>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <utility>
#include <string_view>
#include <locale>
#include <fstream>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <csignal>
#include <thread>
#include <condition_variable>
#include <mutex>
#include "clipboard.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#define isatty _isatty
#define fileno _fileno
#include "windows.hpp"
#else
#include <unistd.h>
#endif

#if defined(X11_AVAILABLE)
#include "x11.hpp"
#endif

#if defined(WAYLAND_AVAILABLE)
#include "wayland.hpp"
#endif

#if defined(__APPLE__)
#include "macos.hpp"
#endif

namespace fs = std::filesystem;

std::string clipboard_name = "0";

std::condition_variable cv;
std::mutex m;
std::atomic<SpinnerState> spinner_state = SpinnerState::Done;
std::thread indicator;

Copying copying;
Filepath filepath;
Successes successes;
IsTTY is_tty;
Action action;

std::array<std::pair<std::string_view, std::string_view>, 8> colors = {{
    {"{red}", "\033[38;5;196m"},
    {"{green}", "\033[38;5;40m"},
    {"{yellow}", "\033[38;5;214m"},
    {"{blue}", "\033[38;5;51m"},
    {"{orange}", "\033[38;5;208m"},
    {"{pink}", "\033[38;5;219m"},
    {"{bold}", "\033[1m"},
    {"{blank}", "\033[0m"}
}};

std::string replaceColors(const std::string_view& str) {
    std::string temp(str); //a string to do scratch work on
    for (const auto& key : colors) { //iterate over all the possible colors to replace
        for (int i = 0; (i = temp.find(key.first, i)) != std::string::npos; i += key.second.length()) {
            temp.replace(i, key.first.length(), key.second);
        }
    }
    return temp;
}

void forceClearTempDirectory() {
    fs::remove(filepath.original_files);
    for (const auto& entry : fs::directory_iterator(filepath.main)) {
        fs::remove_all(entry.path());
    }
}

bool stopIndicator(bool change_condition_variable = true) {
    SpinnerState expect = SpinnerState::Active;
    if (!change_condition_variable) {
        return spinner_state.exchange(SpinnerState::Cancel) == expect;
    }
    if (!spinner_state.compare_exchange_strong(expect, SpinnerState::Done)) {
        return false;
    }
    cv.notify_one();
    indicator.join();
    return true;
}

void setupSignals() {
    signal(SIGINT, [](int dummy) {
        if (!stopIndicator(false)) {
            // Indicator thread is not currently running. TODO: Write an unbuffered newline, and maybe a cancelation message, directly to standard error. Note: There is no standard C++ interface for this, so this requires an OS call.
            _exit(1);
        }
    });
}

void setLocale() {
    try {
        if (std::locale("").name().substr(0, 2) == "es") {
            setLanguageES();
        } else if (std::locale("").name().substr(0, 2) == "pt") {
            setLanguagePT();
        } else if (std::locale("").name().substr(0, 2) == "tr") {
            setLanguageTR();
        }
    } catch (...) {}
}

void showHelpMessage(int& argc, char *argv[]) {
    for (int i = 1; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || (argc >= 2 && !strcmp(argv[1], "help"))) {
            printf(replaceColors(help_message).data(), constants.clipboard_version.data());
            exit(0);
        }
    }
}

void setupItems(int& argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        copying.items.push_back(argv[i]);
    }
}

void setClipboardName(int& argc, char *argv[]) {
    if (argc >= 2) {
        clipboard_name = argv[1];
        if (clipboard_name.find_first_of("_:;|") != std::string::npos) {
            clipboard_name = clipboard_name.substr(clipboard_name.find_first_of("_:;|") + 1);
            copying.use_perma_clip = true;
        } else {
            clipboard_name = clipboard_name.substr(clipboard_name.find_last_not_of("0123456789") + 1);
        }
        if (clipboard_name.empty()) {
            clipboard_name = constants.default_clipboard_name;
        } else {
            argv[1][strlen(argv[1]) - (clipboard_name.length() + copying.use_perma_clip)] = '\0';
        }
    }

    if (argc >= 3) {
        if (!strcmp(argv[2], "-c") && argc >= 4) {
            clipboard_name = argv[3];
            for (int i = 2; i < argc - 2; i++) {
                argv[i] = argv[i + 2];
            }
            argc -= 2;
        } else if (!strncmp(argv[2], "--clipboard=", 12)) {
            clipboard_name = argv[2] + 12;
            for (int i = 2; i < argc - 1; i++) {
                argv[i] = argv[i + 1];
            }
            argc -= 1;
        }
    }

    filepath.temporary = (getenv("TMPDIR") ? getenv("TMPDIR") : fs::temp_directory_path()) / constants.temporary_directory_name / clipboard_name; //set filepath.temporary to TMPDIR if it exists, otherwise use the system's temporary directory

    filepath.persistent = filepath.home / constants.persistent_directory_name / clipboard_name;
    
    filepath.main = copying.use_perma_clip ? filepath.persistent : filepath.temporary;

    filepath.original_files = filepath.main.parent_path() / (clipboard_name + std::string(constants.original_files_extension));
}

void setupVariables(int& argc, char *argv[]) {
    is_tty.std_in = isatty(fileno(stdin));
    is_tty.std_out = isatty(fileno(stdout));
    is_tty.std_err = isatty(fileno(stderr));

    if(getenv("IS_ACTUALLY_A_TTY")) { //add test compatibility where isatty returns false, but there is actually a tty
        is_tty.std_in = true;
        is_tty.std_out = true;
        is_tty.std_err = true;
    }

    #if defined(_WIN64) || defined (_WIN32)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); //Windows terminal color compatibility
	DWORD dwMode = 0;
	GetConsoleMode(hOut, &dwMode);
	if (!SetConsoleMode(hOut, (dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT))) {
        for (auto& key : colors) {
            key.second = "";
        }
	}
	SetConsoleOutputCP(CP_UTF8); //fix broken accents on Windows

    filepath.home = getenv("USERPROFILE");
    #else
    filepath.home = getenv("HOME");
    #endif

    if (getenv("NO_COLOR") && !getenv("FORCE_COLOR")) {
        for (auto& key : colors) {
            key.second = "";
        }
    }
}

void createTempDirectory() {
    fs::create_directories(filepath.temporary);
    fs::create_directories(filepath.persistent);
}

void syncWithGUIClipboard(const std::string& text) {
    forceClearTempDirectory();
    std::ofstream output(filepath.main / constants.pipe_file);
    output << text;
}

void syncWithGUIClipboard(const ClipboardPaths& clipboard) {
    // Only clear the temp directory if all files in the clipboard are outside the temp directory
    // This avoids the situation where we delete the very files we're trying to copy
    auto allOutsideFilepath = std::all_of(clipboard.paths().begin(), clipboard.paths().end(), [](auto& path) {
        auto relative = fs::relative(path, filepath.main);
        auto firstElement = *(relative.begin());
        return firstElement == fs::path("..");
    });

    if (allOutsideFilepath) {
        forceClearTempDirectory();
    }

    for (auto&& path : clipboard.paths()) {
        if (!fs::exists(path)) {
            continue;
        }

        auto target = filepath.main / path.filename();
        if (fs::exists(target) && fs::equivalent(path, target)) {
            continue;
        }

        try {
            fs::copy(path, target, copying.opts | fs::copy_options::create_hard_links);
        } catch (const fs::filesystem_error& e) {
            try {
                fs::copy(path, target, copying.opts);
            } catch (const fs::filesystem_error& e) {
                // Give up
            }
        }
    }

    if (clipboard.action() == ClipboardPathsAction::Cut) {
        std::ofstream originalFiles { filepath.original_files };
        for (auto&& path : clipboard.paths()) {
            originalFiles << path.string() << std::endl;
        }
    }
}

void syncWithGUIClipboard() { 
    if (clipboard_name == constants.default_clipboard_name) { //also check if the system clipboard is newer than filepath.main (check the last write time), and if it is newer, write the contents of the system clipboard to filepath.main
        ClipboardContent guiClipboard;

        #if defined(X11_AVAILABLE) && !defined(NOGUI)
        guiClipboard = getX11Clipboard();
        #endif

        #if defined(WAYLAND_AVAILABLE) && !defined(NOGUI)

        #endif

        #if defined(_WIN32) || defined(_WIN64) && !defined(NOGUI)
        guiClipboard = syncWithWindowsClipboard();
        #elif defined(__APPLE__) && !defined(NOGUI)
        //guiClipboard = syncWithMacClipboard();
        #endif

        if (guiClipboard.type() == ClipboardContentType::Text) {
            syncWithGUIClipboard(guiClipboard.text());
        } else if (guiClipboard.type() == ClipboardContentType::Paths) {
            syncWithGUIClipboard(guiClipboard.paths());
        }
    }
}

void showClipboardStatus() {
    std::vector<std::pair<std::string, bool>> clipboards_with_contents;
    auto iterateClipboards = [&](const fs::path& path, bool persistent) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (fs::is_directory(entry) && !fs::is_empty(entry)) {
                clipboards_with_contents.push_back({entry.path().filename().string(), persistent});
            }
        }
    };
    iterateClipboards(filepath.temporary.parent_path(), false);
    iterateClipboards(filepath.persistent.parent_path(), true);
    std::sort(clipboards_with_contents.begin(), clipboards_with_contents.end());
    if (clipboards_with_contents.empty()) {
        printf("%s", replaceColors(no_clipboard_contents_message).data());
    } else {
        printf("%s", replaceColors(check_clipboard_status_message).data());
        for (int clipboard = 0; clipboard < clipboards_with_contents.size(); clipboard++) {
            printf(replaceColors("{bold}%s%s{blank}{blue}").data(), clipboards_with_contents.at(clipboard).first.data(), clipboards_with_contents.at(clipboard).second ? " (p)" : "");
            if (clipboard != clipboards_with_contents.size() - 1) {
                printf(", ");
            }
        }
        printf("\n");
    }
    printf("%s", replaceColors(clipboard_action_prompt).data());
}

void showClipboardContents() {
    if (fs::is_directory(filepath.main) && !fs::is_empty(filepath.main)) {
        if (fs::is_regular_file(filepath.main / constants.pipe_file)) {
            std::ifstream input(filepath.main / constants.pipe_file);
            std::string line;
            std::getline(input, line, '\0');
            printf(replaceColors(clipboard_text_contents_message).data(), std::min(static_cast<unsigned int>(250), static_cast<unsigned int>(line.size())), clipboard_name.data());
            printf(replaceColors("{bold}{blue}%s\n{blank}").data(), line.substr(0, 250).data());
            if (line.size() > 250) {
                printf(replaceColors(and_more_items_message).data(), line.size() - 250);
            }
            return;
        }
        unsigned int total_items = 0;
        for (const auto& entry : fs::directory_iterator(filepath.main)) {
            total_items++;
        }
        printf(replaceColors(clipboard_item_contents_message).data(), std::min(static_cast<unsigned int>(20), total_items), clipboard_name.data());
        auto it = fs::directory_iterator(filepath.main);
        for (int i = 0; i < std::min(static_cast<unsigned int>(20), total_items); i++) {
            printf(replaceColors("{blue}▏ {bold}%s{blank}\n").data(), it->path().filename().string().data());
            if (i == 19 && total_items > 20) {
                printf(replaceColors(and_more_items_message).data(), total_items - 20);
            }
            it++;
        }
    } else {
        printf(replaceColors(no_clipboard_contents_message).data(), actions[Action::Cut].data(), actions[Action::Copy].data(), actions[Action::Paste].data(), actions[Action::Copy].data());
    }
}

void setupAction(int& argc, char *argv[]) {
    auto flagIsPresent = [&](const std::string_view& flag, const std::string_view& shortcut = ""){
        for (int i = 1; i < argc && strcmp(argv[i], "--"); i++) {
            if (!strcmp(argv[i], flag.data()) || !strcmp(argv[i], (std::string(shortcut).append(flag)).data())) {
                for (int j = i; j < argc - 1; j++) {
                    argv[j] = argv[j + 1];
                }
                argc--;
                return true;
            }
        }
        return false;
    };
    if (argc >= 2) {
        if (flagIsPresent(actions[Action::Cut], "--") || flagIsPresent(action_shortcuts[Action::Cut], "-")) { //replace with join_with_view when C++23 becomes available
            action = Action::Cut;
            if (!is_tty.std_in || !is_tty.std_out) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Copy].data(), actions[Action::Copy].data());
                exit(1);
            }
        } else if (flagIsPresent(actions[Action::Copy], "--") || flagIsPresent(action_shortcuts[Action::Copy], "-")) {
            action = Action::Copy;
            if (!is_tty.std_in) {
                action = Action::PipeIn;
            } else if (!is_tty.std_out) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Paste].data(), actions[Action::Paste].data());
                exit(1);
            }
        } else if (flagIsPresent(actions[Action::Paste], "--") || flagIsPresent(action_shortcuts[Action::Paste], "-")) {
            action = Action::Paste;
            if (!is_tty.std_out) {
                action = Action::PipeOut;
            } else if (!is_tty.std_in) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Copy].data(), actions[Action::Copy].data());
                exit(1);
            }
        } else if (flagIsPresent(actions[Action::Show], "--") || flagIsPresent(action_shortcuts[Action::Show], "-")) {
            action = Action::Show;
        } else if (flagIsPresent(actions[Action::Clear], "--") || flagIsPresent(action_shortcuts[Action::Clear], "-")) {
            action = Action::Clear;
            if (!is_tty.std_in) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Cut].data(), actions[Action::Cut].data());
                exit(1);
            } else if (!is_tty.std_out) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Paste].data(), actions[Action::Paste].data());
                exit(1);
            }
        } else if (flagIsPresent("ee")) {
            printf("%s", replaceColors("{bold}{blue}https://youtu.be/Lg_Pn45gyMs\n{blank}").data());
            exit(0);
        } else {
            printf(replaceColors(no_valid_action_message).data(), argv[1]);
            exit(1);
        }
        if (flagIsPresent("--fast-copy") || flagIsPresent("-fc")) {
            copying.use_safe_copy = false;
        }
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--")) {
                for (int j = i; j < argc; j++) {
                    argv[j] = argv[j + 1];
                }
                argc--;
                break;
            }
        }
    } else if (!is_tty.std_in) {
        action = Action::PipeIn;
    } else if (!is_tty.std_out) {
        action = Action::PipeOut;
    } else {
        showClipboardStatus();
        exit(0);
    }
    if (action == Action::PipeIn || action == Action::PipeOut) {
        if (argc >= 3) {
            fprintf(stderr, "%s", replaceColors(redirection_no_items_message).data());
            exit(1);
        }
    }
}

void checkForNoItems() {
    if ((action == Action::Cut || action == Action::Copy) && copying.items.size() < 1) {
        printf(replaceColors(choose_action_items_message).data(), actions[action].data(), actions[action].data(), actions[action].data());
        exit(1);
    }
    if (action == Action::Paste && fs::is_empty(filepath.main)) {
        showClipboardStatus();
        exit(0);
    }
}

void setupIndicator() {
    std::unique_lock<std::mutex> lock(m);
    int output_length = 0;
    const std::array<std::string_view, 10> spinner_steps{"━       ", "━━      ", " ━━     ", "  ━━    ", "   ━━   ", "    ━━  ", "     ━━ ", "      ━━", "       ━", "        "};
    static unsigned int percent_done = 0;
    if ((action == Action::Cut || action == Action::Copy) && is_tty.std_err) {
        static unsigned long items_size = copying.items.size();
        for (int i = 0; spinner_state == SpinnerState::Active; i == 9 ? i = 0 : i++) {
            percent_done = ((successes.files + successes.directories + copying.failedItems.size()) * 100) / items_size;
            output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), percent_done, "%", spinner_steps.at(i).data());
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&]{ return spinner_state != SpinnerState::Active; });
        }
    } else if ((action == Action::PipeIn || action == Action::PipeOut) && is_tty.std_err) {
        for (int i = 0; spinner_state == SpinnerState::Active; i == 9 ? i = 0 : i++) {
            output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), static_cast<int>(successes.bytes), "B", spinner_steps.at(i).data());
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&]{ return spinner_state != SpinnerState::Active; });
        }
    } else if (action == Action::Paste && is_tty.std_err) {
        static unsigned long items_size = 0;
        if (items_size == 0) {
            for (const auto& f : fs::directory_iterator(filepath.main)) {
                items_size++;
            }
            if (items_size == 0) {
                items_size = 1;
            }
        }
        for (int i = 0; spinner_state == SpinnerState::Active; i == 9 ? i = 0 : i++) {
            percent_done = ((successes.files + successes.directories + copying.failedItems.size()) * 100) / items_size;
            output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), percent_done, "%", spinner_steps.at(i).data());
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&]{ return spinner_state != SpinnerState::Active; });
        }
    } else if (is_tty.std_err) {
        while (spinner_state == SpinnerState::Active) {
            output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), 0, "%", "");
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&]{ return spinner_state != SpinnerState::Active; });
        }
    }
    if (is_tty.std_err) {
        fprintf(stderr, "\r%*s\r", output_length, "");
    }
    if (spinner_state == SpinnerState::Cancel) {
        fprintf(stderr, replaceColors(cancelled_message).data(), actions[action].data());
        fflush(stderr);
        _exit(1);
    }
    fflush(stderr);
}

void startIndicator() { // If cancelled, leave cancelled
    SpinnerState expect = SpinnerState::Done;
    spinner_state.compare_exchange_strong(expect, SpinnerState::Active);
    indicator = std::thread(setupIndicator);
}

void deduplicateItems() {
    std::sort(copying.items.begin(), copying.items.end());
    copying.items.erase(std::unique(copying.items.begin(), copying.items.end()), copying.items.end());
}

unsigned long long calculateTotalItemSize() {
    unsigned long long total_item_size = 0;
    for (const auto& i : copying.items) {
        try {
            if (fs::is_directory(i)) {
                for (const auto& entry : fs::recursive_directory_iterator(i)) {
                    if (fs::is_regular_file(entry)) {
                        total_item_size += fs::file_size(entry);
                    } else {
                        total_item_size += 16;
                    }
                }
            } else if (fs::is_regular_file(i)) {
                total_item_size += fs::file_size(i);
            } else {
                total_item_size += 16;
            }
        } catch (const fs::filesystem_error& e) {
            copying.failedItems.emplace_back(i.string(), e.code());
        }
    }   
    return total_item_size;
}

void checkItemSize() {
    const unsigned long long space_available = fs::space(filepath.main).available;
    unsigned long long total_item_size = 0;
    if (action == Action::Cut || action == Action::Copy) {
        total_item_size = calculateTotalItemSize();
        if (total_item_size > (space_available / 2)) {
            stopIndicator();
            fprintf(stderr, replaceColors(not_enough_storage_message).data(), total_item_size / 1024.0, space_available / 1024.0);
            exit(1);
        }
    }
}

void clearTempDirectory() {
    if (action != Action::Paste) {
        fs::remove(filepath.original_files);
    }
    if (action == Action::Copy || action == Action::Cut || action == Action::PipeIn || action == Action::Clear) {
        for (const auto& entry : fs::directory_iterator(filepath.main)) {
            fs::remove_all(entry.path());
        }
    }
}

void copyFiles() {
    std::ofstream originalFiles;
    if (action == Action::Cut) {
        originalFiles.open(filepath.original_files);
    }
    for (const auto& f : copying.items) {
        auto copyItem = [&](const bool use_regular_copy = copying.use_safe_copy) {
            if (fs::is_directory(f)) {
                if (f.filename() == "") {
                    fs::create_directories(filepath.main / f.parent_path().filename());
                    fs::copy(f, filepath.main / f.parent_path().filename(), copying.opts);
                } else {
                    fs::create_directories(filepath.main / f.filename());
                    fs::copy(f, filepath.main / f.filename(), copying.opts);
                }
                successes.directories++;
            } else {
                fs::copy(f, filepath.main / f.filename(), use_regular_copy ? copying.opts : copying.opts | fs::copy_options::create_hard_links);
                successes.files++;
            }
            if (action == Action::Cut) {
                originalFiles << fs::absolute(f).string() << std::endl;
            }
        };
        try {
            copyItem();
        } catch (const fs::filesystem_error& e) {
            if (!copying.use_safe_copy) {
                try {
                    copyItem(true);
                } catch (const fs::filesystem_error& e) {
                    copying.failedItems.emplace_back(f.string(), e.code());
                }
            } else {
                copying.failedItems.emplace_back(f.string(), e.code());
            }
        }
    }
}

void removeOldFiles() {
    if (fs::is_regular_file(filepath.original_files)) {
        std::ifstream files(filepath.original_files);
        std::string line;
        while (std::getline(files, line)) {
            try {
                fs::remove_all(line);
            } catch (const fs::filesystem_error& e) {
                copying.failedItems.emplace_back(line, e.code());
            }
        }
        files.close();
        if (copying.failedItems.empty()) {
            fs::remove(filepath.original_files);
        }
        action = Action::Cut;
    }
}

bool userIsARobot() {
    return !is_tty.std_err || !is_tty.std_in || !is_tty.std_out || getenv("CI");
}

int getUserDecision(const std::string& item) {
    if (userIsARobot()) {
        return 2;
    }
    fprintf(stderr, replaceColors(item_already_exists_message).data(), item.data());
    std::string decision;
    while (true) {
        std::getline(std::cin, decision);
        fprintf(stderr, "%s", replaceColors("{blank}").data());
        if (decision == "y" || decision == "yes") {
            return 1;
        } else if (decision == "ya" || decision == "yesall") {
            return 2;
        } else if (decision == "n" || decision == "no") {
            return -1;
        } else if (decision == "na" || decision == "noall") {
            return -2;
        } else {
            fprintf(stderr, "%s", replaceColors(bad_response_message).data());
        }
    }
}

void pasteFiles() {
    int user_decision = 0;
    for (const auto& f : fs::directory_iterator(filepath.main)) {
        auto pasteItem = [&](const bool use_regular_copy = copying.use_safe_copy) {
            if (fs::exists(fs::current_path() / f.path().filename()) && fs::equivalent(f, fs::current_path() / f.path().filename())) {
                if (fs::is_directory(f)) {
                    successes.directories++;
                } else {
                    successes.files++;
                }
                return;
            }
            if (fs::is_directory(f)) {
                fs::copy(f, fs::current_path() / f.path().filename(), copying.opts);
                successes.directories++;
            } else {
                fs::copy(f, fs::current_path() / f.path().filename(), use_regular_copy ? copying.opts : copying.opts | fs::copy_options::create_hard_links);
                successes.files++;
            }
        };
        try {
            if (fs::exists(fs::current_path() / f.path().filename())) {
                switch (user_decision) {
                    case -2:
                        break;
                    case -1:
                    case 0:
                    case 1:
                        stopIndicator();
                        user_decision = getUserDecision(f.path().filename().string());
                        startIndicator();
                        break;
                    case 2:
                        pasteItem();
                        break;
                }
                switch (user_decision) {
                    case -1:
                        break;
                    case 1:
                        pasteItem();
                }
            } else {
                pasteItem();
            }
        } catch (const fs::filesystem_error& e) {
            if (!copying.use_safe_copy) {
                try {
                    pasteItem(true);
                } catch (const fs::filesystem_error& e) {
                    copying.failedItems.emplace_back(f.path().filename().string(), e.code());
                }
            } else {
                copying.failedItems.emplace_back(f.path().filename().string(), e.code());
            }
        }
    }
    removeOldFiles();
}

void pipeIn() {
    std::ofstream file(filepath.main / constants.pipe_file);
    std::string buffer;
    std::string line;
    for (int i = 0; std::getline(std::cin, line); i == 19 ? i = 0 : i++) {
        buffer += line + "\n";
        successes.bytes += line.size() + 1;
        if (i == 19) {
            file << buffer;
            buffer = "";
        }
    }
    if (buffer != "") {
        file << buffer;
    }
    file.close();
}

void pipeOut() {
    std::string line;
    for (const auto& entry : fs::recursive_directory_iterator(filepath.main)) {
        std::ifstream file(entry.path());
        while (std::getline(file, line)) {
            printf("%s\n", line.data());
            successes.bytes += line.size() + 1;
        }
        file.close();
    }
}

void clearClipboard() {
    if (fs::is_empty(filepath.main)) {
        printf("%s", replaceColors(clear_success_message).data());
    } else {
        printf("%s", replaceColors(clear_fail_message).data());
    }
}

void performAction() {
    switch (action) {
        case Action::Copy:
        case Action::Cut:
            copyFiles();
            break;
        case Action::Paste:
            pasteFiles();
            break;
        case Action::PipeIn:
            pipeIn();
            break;
        case Action::PipeOut:
            pipeOut();
            break;
        case Action::Clear:
            clearClipboard();
            break;
        case Action::Show:
            showClipboardContents();
            break;
    }
}

void updateGUIClipboard() {
    #if defined(_WIN32) || defined(_WIN64)
    updateWindowsClipboard();
    #endif
}


void showFailures() {
    if (copying.failedItems.size() > 0) {
        printf(replaceColors(clipboard_failed_message).data(), actions[action].data());
        for (int i = 0; i < std::min(5, static_cast<int>(copying.failedItems.size())); i++) {
            printf(replaceColors("{red}▏ {bold}%s{blank}{red}: %s{blank}\n").data(), copying.failedItems.at(i).first.data(), copying.failedItems.at(i).second.message().data());
            if (i == 4 && copying.failedItems.size() > 5) {
                printf(replaceColors(and_more_fails_message).data(), int(copying.failedItems.size() - 5));
            }
        }
        printf("%s", replaceColors(fix_problem_message).data());
    }
}

void showSuccesses() {
    if (action == Action::PipeIn || action == Action::PipeOut && is_tty.std_err) {
        fprintf(stderr, replaceColors(pipe_success_message).data(), did_action[action].data(), static_cast<int>(successes.bytes));
        return;
    }
    if ((successes.files == 1 && successes.directories == 0) || (successes.files == 0 && successes.directories == 1)) {
        if (action == Action::Paste) {
            printf("%s", replaceColors(paste_success_message).data());
        } else {
            printf(replaceColors(one_item_success_message).data(), did_action[action].data(), copying.items.at(0).string().data());
        }
    } else {
        if ((successes.files > 1) && (successes.directories == 0)) {
            printf(replaceColors(multiple_files_success_message).data(), did_action[action].data(), static_cast<int>(successes.files));
        } else if ((successes.files == 0) && (successes.directories > 1)) {
            printf(replaceColors(multiple_directories_success_message).data(), did_action[action].data(), static_cast<int>(successes.directories));
        } else if ((successes.files >= 1) && (successes.directories >= 1)) {
            printf(replaceColors(multiple_files_directories_success_message).data(), did_action[action].data(), static_cast<int>(successes.files), static_cast<int>(successes.directories));
        }
    }
}

int main(int argc, char *argv[]) {
    try {
        setupSignals();

        setupVariables(argc, argv);

        setLocale();

        setClipboardName(argc, argv);

        showHelpMessage(argc, argv);

        createTempDirectory();

        syncWithGUIClipboard();

        setupAction(argc, argv);

        setupItems(argc, argv);

        checkForNoItems();

        startIndicator();

        deduplicateItems();

        checkItemSize();

        clearTempDirectory();

        performAction();

        if (action == Action::Cut || action == Action::Copy || action == Action::PipeIn || action == Action::Clear) { //only update GUI clipboard on write operations
            updateGUIClipboard();
        }

        stopIndicator();

        showFailures();

        showSuccesses();
    } catch (const std::exception& e) {
        if (stopIndicator()) {
            fprintf(stderr, replaceColors(internal_error_message).data(), e.what());
        }
        exit(1);
    }
    return 0;
}
