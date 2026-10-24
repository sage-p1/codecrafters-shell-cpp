#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <tuple>
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <readline/readline.h>
#include <readline/history.h>
#include <climits>
#include <cctype>

/* 

Global variables 

*/

// Declare machine-agnostic os_separator for different OSes
#ifdef _WIN32
const char os_separator {';'};
#else
const char os_separator {':'};
#endif

// Global separator used to handle all stringstreams
const char ss_separator { '\r' };

// Global boolean that signifies when a second is pressed to list all autocomplete options
bool double_tab { false };

// Background job map mapping job id to a pair of pid and command
std::unordered_map<int, std::pair<pid_t, std::string>> job_map {};
// Priority queue of used job ids for recycling (minimum ids first)
std::set<int> done_job_ids {};
int highest_job { 0 }; // Tracks the highest running job id

// Global completer map to match -> command : /path/to/executable
std::unordered_map<std::string, std::string> complete_map {};

// Global vector for storing old commands
std::vector<std::string> history_vector {};

// Global index to cycle through previous commands
int history_index = history_vector.size();

// Index of the last time append was done to avoid sending the same commands several times
int last_append { 0 };

// 
std::unordered_map<std::string, std::string> declare_map {};


/*

Function declarations for simple builtins and command logic

*/

// Match command to builtin type
bool is_builtin(std::string command);

// Built in function to handle echo and various redirection options
void echo(std::string to_print);

// Type function to categorize commands
void type(std::string content);

/*

Function declarations for string parsing

*/

// Remove trailing whitespace (and optionally newlines) at the start and end of word
void trim(std::string &word, bool ignore_newline);

// If a backslash is found, escape the following character
void process_char(
    bool escape, 
    std::string &term, 
    std::string &line, int &i
);

// Capture a sequence of characters contained within a pair of quotes (single or double)
void capture_sequence(
    char quote, 
    bool escape,
    std::string &term, 
    std::string &line, int &i
);

// Parses single-quotes, double-quotes, and whitespace
std::stringstream parse_quotes(std::string line);

// Write the given content to file
void write_to_file(
    std::string filename,
    std::string content, 
    std::ios_base::openmode mode
);

/*

Function declarations for directory navigation

*/

// Displays current working directory
void pwd();

// Change the current working directory
void cd(std::string path);

// In the event command is not builtin, search for the executable
void find_and_execute(std::string line);

// Loads all paths from $PATH
std::string load_paths();

// Determine if executable with permissions exists
std::tuple<bool, std::string> find_executable(
    std::string &all_paths, 
    std::string &command
);

/*

Function declarations for complete command

*/

// Main function to execute code for the complete builtin 
void complete(std::string content);

// Autocomplete the current string
int rl_autocomplete(int count, int key);

// Write autocompleted content to terminal
void add_to_rl_line_buffer(char *completed, int size);

// Execute the saved complete script associated with the given command
void run_complete_script(
    std::string &command,
    std::string &word,
    std::string &to_complete
);

// Autocomplete files and directories
void autocomplete_files_and_dirs(int start);

// Autocomplete executable command
void autocomplete_executables();

// Determine if given path exists and is executable
bool executable_exists_with_perms(std::string &path);

// Tracks executable/file if it matches current keyboard state (and is not already included)
int add_match(
    std::string &path,
    std::string &to_match, 
    std::vector<std::string> &all_matches
);


/*

Function declarations for jobs command

*/

// If a command ends with '&', run it in the background
void run_background_job(std::string line);

// Add all finished background jobs to separate set for tracking
void find_exited_jobs();

// Display background jobs; either all for "jobs" command or just completed ones
void display_jobs(
    bool show_all
);

/*

Function declarations for pipelining

*/

// Function to start the entire pipeline of commands
void start_pipeline(
    int pipe_start,
    std::string &line, 
    std::vector<std::string> pipeline_commands
);

// Run one pipeline command
void run_pipeline_command(
    int *fds_p,
    int position,
    std::vector<std::string> &pipeline_commands
);

/*

Function declarations for history command

*/

// Display all previously entered commands
void history(std::string &content);

// Read history from file into main memory
void read_history(std::string &read_file_name);

// Write history from memory into file
void write_history(std::string &write_file_name);

// Write history to file after shell exits
void write_history_on_exit();

// Cycle through previous commands 
int rl_up_arrow(int count, int key);

// Sift through newer commands
int rl_down_arrow(int count, int key);

/*

Function declarations for parameter expansion

*/

// Declare command for adding shell variables
void declare(std::string content);

// Expand command with declared variables
std::string expand(std::string &line);





int main(int argc, char *argv[]) { 
    // Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    // Bind tab to auto-complete
    rl_bind_key('\t', rl_autocomplete);

    // Bind to up and down arrow keys to cycle through old commands
    // https://stackoverflow.com/questions/39969495/binding-to-arrow-keys-in-readline-library?rq=3
    rl_generic_bind(ISFUNC, "\\e[A", (char *)rl_up_arrow, rl_get_keymap());
    rl_generic_bind(ISFUNC, "\\e[B", (char *)rl_down_arrow, rl_get_keymap());

    // Load history file environment variable from memory
    char *c_read_file_name { getenv("HISTFILE") }; 
    if (c_read_file_name != nullptr) {
        std::string read_file_name { c_read_file_name }; // Convert to string
        if (std::filesystem::exists(read_file_name)) {
            read_history(read_file_name);
        } 
    }

    // Loop shell until user exits 
    while (true) {
        // Display shell line 
        std::string line = readline("$ ");

        // Add line to history
        history_vector.push_back(line);

        // Expand all variables
        line = expand(line);

        // Parse line for pipes
        std::vector<std::string> pipeline_commands {};
        int pipe_start { 0 };
        for (int i { 0 }; i < line.size(); i++) {
            if (line[i] == '|') {
                pipeline_commands.push_back(line.substr(pipe_start, i - pipe_start));
                pipe_start = i + 1;
            } 
        }

        // Create pipes between commands
        if (pipeline_commands.size() > 0) {
            start_pipeline(pipe_start, line, pipeline_commands);
        }

        // Check if command is to be run in the background
        else if (line[line.size() - 1] == '&') {
            run_background_job(line);
        }

        // End loop if user exits
        else if (line.substr(0, 4) == "exit") {
            write_history_on_exit();
            break;
        } 
        
        // Echo whatever user sent back to the terminal
        else if (line.substr(0, 5) == "echo ") {
            echo(line.substr(5));
        } 
        
        // Determine type of every passed command
        else if (line.substr(0, 5) == "type ") {
            type(line.substr(5));
        } 

        // Print current working directory
        else if (line == "pwd") {
            pwd();
        }

        else if (line.substr(0, 3) == "cd ") {
            cd(line.substr(3));
        }

        else if (line.substr(0, 9) == "complete ") {
            complete(line.substr(9));
        }
        
        else if (line.substr(0, 4) == "jobs") {
            // Determine all exited jobs and print all to terminal
            find_exited_jobs();
            display_jobs(true);
        }

        else if (line.substr(0, 7) == "history") {
            std::string content { "" };
            if (line.size() > 7) {
                content = line.substr(8);
            }
            history(content); 
        }

        else if (line.substr(0, 7) == "declare") {
            declare(line.substr(7));
        }

        // Print if the command is not recognized
        else {
            find_and_execute(line);
        }

        // Reap before next prompt that isn't "jobs" 
        if (line != "jobs") {
            find_exited_jobs();
            display_jobs(false);
        }

        // Reset history after each 
        history_index = history_vector.size();
    }
}






/*

Function definitions for simple builtins and command logic

*/

// Match command to builtin type
bool is_builtin(std::string command) {
    return command == "type" || 
        command == "exit" || 
        command == "echo" ||
        command == "pwd" ||
        command == "cd" ||
        command == "complete" ||
        command == "jobs" ||
        command == "history" ||
        command == "declare";
}

// Built in function to handle echo and various redirection options
void echo(std::string to_print) {
    std::stringstream ss { parse_quotes(to_print) };
    std::string output;
    std::string content { "" };
    bool redirect_stdout { false };
    while (!redirect_stdout && std::getline(ss, output, ss_separator)) {
        // Redirect stdout
        if (output == ">" || output == "1>") {
            std::string out_file;
            std::getline(ss, out_file, ss_separator);
            write_to_file(out_file, content, std::ios_base::out);
            redirect_stdout = true;
            break;
        } 

        // Redirect and append stdout
        else if (output == ">>" || output == "1>>") {
            std::string out_file;
            std::getline(ss, out_file, ss_separator);
            write_to_file(out_file, content, std::ios_base::app);
            redirect_stdout = true;
            break;
        }  
        
        // Redirect stderr
        else if (output == "2>") {
            // Redirect stderr toward user-given file
            std::string err_file;
            std::getline(ss, err_file, ss_separator);
            char *c_err_file = err_file.data();
            int err { open(c_err_file, O_WRONLY | O_CREAT, 0666) };
            dup2(err, STDERR_FILENO);
            break;
        }

        // Redirect and append stderr
        else if (output == "2>>") {
            // Redirect stderr toward user-given file
            std::string err_file;
            std::getline(ss, err_file, ss_separator);
            char *c_err_file = err_file.data();
            int err { open(c_err_file, O_WRONLY | O_CREAT | O_APPEND, 0666) };
            dup2(err, STDERR_FILENO);
            break;
        }

        content += output + ' ';
    }
    if (!redirect_stdout) {
        content[content.size() - 1] = '\n';
        std::cout << content;
    }
}

// Type function to categorize commands
void type(std::string content) {
    std::stringstream ss { parse_quotes(content)  };
    std::string command;
    while (std::getline(ss, command, ss_separator)) {
        // First, check if the command is builtin
        if (is_builtin(command)) {
            std::cout << command << " is a shell builtin\n";
        } 
    
        // Second, check to see if line is in $PATH
        else {
            // Get full $PATH from env
            std::string all_paths { load_paths() }; 

            // Get bool that determines if file exists and string of the executable path
            std::tuple<bool, std::string> executable_data = find_executable(all_paths, command);
            bool exists { std::get<0>(executable_data) };
            std::string executable { std::get<1>(executable_data) };

            // Print executable if it exists and has owner execute permissions
            // Skip it if it exists but does not have execute permissions
            // Print if it cannot be found
            if (exists && !executable.empty()) {
                std::cout << command << " is " << executable << "\n";
            } else if (!exists) {
                std::cout << command << ": not found\n";
            }
        }
    }
}






/*

Function definitions for string parsing

*/

// Remove trailing whitespace (and optionally newlines) at the start and end of word
void trim(std::string &word, bool ignore_newline) {
    int start { 0 };
    while (start < word.size() && ( std::isspace(word[start]) || (ignore_newline && word[start] == '\n') ) ) {
        start++;
    }
    int end = word.size() - 1;
    while (end >= 0 && ( std::isspace(word[end]) || (ignore_newline && word[start] == '\n') ) ) {
        end--;
    }
    if (start > 0 || end < word.size() - 1) {
        word.replace(0, word.size(), word.substr(start, end - start + 1));
    }
}

// If a backslash is found, escape the following character
void process_char(
    bool escape, 
    std::string &term, 
    std::string &line, int &i
) {
    if (escape && line[i] == '\\' && i+1 < line.size()) {
        i++;
    }
    term += line[i];
    i++;
};

// Capture a sequence of characters contained within a pair of quotes (single or double)
void capture_sequence(
    char quote, 
    bool escape,
    std::string &term, 
    std::string &line, int &i
) {
    while (line[i] != quote && i < line.size()) {
        process_char(escape, term, line, i);
    }
    i++;
}

// Parses single-quotes, double-quotes, and whitespace
std::stringstream parse_quotes(std::string line) {
    trim(line, false);
    std::string term  { "" };
    std::stringstream ss {};
    int i { 0 };
    while (i < line.size()) {
        // Escape all characters detected between single quotes
        if (line[i] == '\'') {
            i++;
            capture_sequence('\'', false, term, line, i);
        } 

        // Escape all characters detected between double quotes
        else if (line[i] == '\"') {
            i++;
            capture_sequence('\"', true, term, line, i);
        } 

        // Whitespace is the only indicator of separate arguments in a line
        // Without quotes, whitespace should be collapsed
        else if (std::isspace(line[i])) {
            if (!term.empty()) {
                ss << term;
                ss << ss_separator;
            }
            while (i+1 < line.size() && std::isspace(line[i+1])) {
                i++;
            }
            term = "";
            i++;
        } 

        else {
            process_char(true, term, line, i);
        }
    }

    // If it exists, add ending term
    if (!term.empty()) {
        ss << term;
        ss << ss_separator;
    }
    return ss;
} 

// Write the given content to file
void write_to_file(
    std::string filename,
    std::string content, 
    std::ios_base::openmode mode
) {
    std::ofstream file;
    file.open(filename, mode);
    file << content << '\n';
    file.close();
}






/*

Function definitions for directory navigation

*/

// Displays current working directory
void pwd() {
    std::string p { std::filesystem::current_path() };
    std::cout << p << "\n";
}

// Change the current working directory
void cd(std::string path) {
    if (path.substr(0, 1) == "~") {
        char *c_home { getenv("HOME") };                                                       
        path = c_home;
    }
    std::error_code ec {};
    std::filesystem::current_path(path, ec);
    if (ec.value() != 0 && !path.empty()) {
        std::cout << "cd: " << path << ": No such file or directory\n";
    }
}

// In the event command is not builtin, search for the executable
void find_and_execute(std::string line) {
    // Get full $PATH from env
    std::string all_paths { load_paths() }; 

    // Parse command from line
    std::stringstream ss { parse_quotes(line)  };
    std::string command;
    std::getline(ss, command, ss_separator);

    // Get bool that determines if file exists and string of the executable path
    std::tuple<bool, std::string> executable_data { find_executable(all_paths, command) };
    bool exists { std::get<0>(executable_data) };
    std::string executable { std::get<1>(executable_data) };

    // Execute commands if it exists and has owner execute permissions
    // Skip it if it exists but does not have execute permissions
    // Print if it cannot be found
    if (exists && !executable.empty()) {
        // Create args array to pass to with command
        char *c_command { command.data() };
        std::vector<std::string> args {};
        std::string arg {};
        while (std::getline(ss, arg, ss_separator)) {
            args.push_back(arg);
        }

        // Collect file names for output or error redirection
        char *out_file { (char *) "" };
        char *err_file { (char *) "" }; 

        // Convert strings to c-style strings
        char *c_args[16]; // Allow up to 16 arguments
        c_args[0] = c_command;
        int i = 0;
        bool append_out { false };
        bool append_err { false };
        while (i < args.size()) {
            char *c_arg = args[i].data();
            // Process arguments for output/error handling
            if (i+1 < args.size()) {
                if (strcmp(c_arg, ">") == 0 || strcmp(c_arg, "1>") == 0) {
                    out_file = args[i+1].data();
                    break;
                } else if (strcmp(c_arg, ">>") == 0 || strcmp(c_arg, "1>>") == 0) {
                    out_file = args[i+1].data();
                    append_out = true;
                    break;
                } else if (strcmp(c_arg, "2>") == 0) {
                    err_file = args[i+1].data();
                    break;
                } else if (strcmp(c_arg, "2>>") == 0) {
                    err_file = args[i+1].data();
                    append_err = true;
                    break;
                }    
            } 
            c_args[i+1] = c_arg;
            i++;
        }
        c_args[i+1] = nullptr;

        // Create fork that execvp will begin and exit from
        pid_t pid { fork() };

        // Fork failure
        if (pid == -1) {
            std::cerr << "Error: fork() failed - " << strerror(errno) << "\n";
            exit(1);
        }

        // Child process that begins executable
        else if (pid == 0) {
            // Redirect stdout toward user-given file
            if (strlen(out_file) > 0) {
                int out;
                if (!append_out) {
                    out = open(out_file, O_WRONLY | O_CREAT, 0666);
                } else {
                    out = open(out_file, O_WRONLY | O_CREAT | O_APPEND, 0666);
                }
                dup2(out, STDOUT_FILENO);
            }

            // Redirect stderr toward user-given file
            if (strlen(err_file) > 0) {
                int err;
                if (!append_err) {
                    err = open(err_file, O_WRONLY | O_CREAT, 0666);
                } else {
                    err = open(err_file, O_WRONLY | O_CREAT | O_APPEND, 0666);
                }
                dup2(err, STDERR_FILENO);
            }

            // Execute external command
            execvp(c_command, c_args);

            // Error if execvp did not execute correctly
            std::cerr << "Error: execvp() failed (4) - " << strerror(errno) << "\n";
            _exit(1);
        } 

        else {
            // Wait for child process to terminate
            int status;
            pid_t waited_pid { waitpid(pid, &status, 0) };
            if (waited_pid == -1) {
                std::cerr << "Error: waitpid() failed - " << strerror(errno) << "\n";
                exit(1);
            }
        }

    } else if (!exists) {
        std::cout << command << ": command not found\n";
    }
}

// Loads all paths from $PATH
std::string load_paths() {
    char *c_all_paths { getenv("PATH") };                                                       
    std::string all_paths { "" };                                                           
    if (c_all_paths != nullptr) {                                                                 
        all_paths = c_all_paths;                                                                    
    }  
    return all_paths;
}

// Determine if executable with permissions exists
std::tuple<bool, std::string> find_executable(
    std::string &all_paths, 
    std::string &command
) {
    // Parse every path in $PATH
    std::stringstream ss(all_paths);
    std::string path;
    std::string executable { "" };
    bool exists { false };

    // Loop over each path in $PATH
    while (std::getline(ss, path, os_separator)) {
        path += "/" + command;
        std::filesystem::path p { path };
        std::filesystem::file_status s { status(p) };
        std::filesystem::perms file_perms { s.permissions() };

        // Confirm file exists
        if (std::filesystem::exists(s)) {
            exists = true;

            // Confirm file has owner execute permissions
            if ((file_perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) {
                executable = path;
                break; // Only print one example even if command qualifies for several
            }
        }
    }
    return std::make_tuple(exists, executable);
}






/*

Function definitions for complete command

*/

// Main function to execute code for the complete builtin 
void complete(std::string content) {
    // Process all arguments and place inside vector
    std::stringstream ss { parse_quotes(content) };
    std::string output;
    std::vector<std::string> arguments {};
    while (std::getline(ss, output, ss_separator)) {
        arguments.push_back(output);
    }
    int command_index = arguments.size() - 1;
    std::string command { arguments[command_index] };
    // Iterate over arguments and process accordingly
    for (int i = 0; i < command_index; i++) {
        // Match path to argument
        if (arguments[i].compare("-C") == 0 && i + 1 < command_index) {
            complete_map[command] = arguments[i+1];
        }
        // Display saved paths (or lack thereof)
        else if (arguments[i].compare("-p") == 0) {
            if (complete_map.find(command) != complete_map.end()) {
                std::cout << "complete -C '" << complete_map[command] << "' " << command << '\n';
            } else {
                std::cout << "complete: " << command << ": no completion specification\n";
            }
        }
        // Remove any saved paths
        else if (arguments[i].compare("-r") == 0) {
            if (complete_map.contains(arguments[i+1])) {
                complete_map.erase(arguments[i+1]);
            }
        }
    }
}

// Autocomplete the current string
int rl_autocomplete(int count, int key) {
    // Check if string is exit
    if (strcmp(rl_line_buffer, "ex") == 0 || strcmp(rl_line_buffer, "exi") == 0) {
        add_to_rl_line_buffer((char *) "exit ", 5);
        double_tab = false;
    } 
    // Check if string is echo
    else if (strcmp(rl_line_buffer, "ec") == 0 || strcmp(rl_line_buffer, "ech") == 0) {
        add_to_rl_line_buffer((char *) "echo ", 5);
        double_tab = false;
    }
    else {
        // Find first space and parse the command to run
        int first_space { -1 };
        for (int i = 0; i < rl_end; i++) {
            if (rl_line_buffer[i] == ' ') {
                first_space = i;
                break;
            }
        }
        int second_space { -1 };
        for (int i = first_space + 1; i < rl_end; i++) {
            if (rl_line_buffer[i] == ' ') {
                second_space = i;
                break;
            }
        }
        std::string full_string { rl_line_buffer };
        std::string command { full_string.substr(0, first_space)};
        // Run saved complete script to generate autocompletes
        if (first_space > -1 && complete_map.find(command) != complete_map.end()) {
            // Parse word to be completed and preceding word
            std::string word { "" };
            std::string to_complete { "" };
            if (second_space > 0) {
                word = full_string.substr(first_space + 1, second_space - first_space - 1);
                trim(word, false);
                if (second_space + 1 < full_string.size()) {
                    to_complete = full_string.substr(second_space + 1);
                    trim(to_complete, false);
                }
            } else if (first_space + 1 < full_string.size()) {
                to_complete = full_string.substr(first_space + 1);
            }

            run_complete_script(command, word, to_complete);
        }
        
        else {
            // Determine if line buffer has a space
            // Detecting a space indicates there is a file/directory to complete
            // Otherwise, an executable is to be completed
            int last_space { -1 };
            for (int i = rl_end - 1; i >= 0; i--) {
                if (rl_line_buffer[i] == ' ') {
                    last_space = i;
                    break;
                }
            }
            if (last_space > -1) {
                autocomplete_files_and_dirs(last_space+1);
            }
            else {
                autocomplete_executables();
            }
        }
    }
    return 0;
}

// Write autocompleted content to terminal
void add_to_rl_line_buffer(char *completed, int size) {
    strncpy(rl_line_buffer, completed, size); // Place completed string on buffer
    rl_line_buffer[size] = '\0'; // End string
    rl_end = size; // Update end to fully print buffer
    rl_point = size; // Update cursor to be after command
}

// Execute the saved complete script associated with the given command
void run_complete_script(
    std::string &command,
    std::string &word,
    std::string &to_complete
) {
    // Convert executable to c_string for execvp
    std::string script = complete_map[command];

    // Set COMP_LINE and COMP_POINT (line before completion)
    std::string comp_line = command + ' ';
    if (!word.empty()) {
        comp_line += word + ' ';
    }
    comp_line += to_complete;
    std::string comp_point = std::to_string(rl_point);
    setenv("COMP_LINE", comp_line.data(), 1);
    setenv("COMP_POINT", comp_point.data(), 1);

    // Execute script
    std::string process_word { word };
    if (word.empty() && !to_complete.empty()) {
        process_word = command;
    }
    char *c_script { script.data() };
    char *c_command { command.data() };
    char *c_to_complete { to_complete.data() };
    char *c_word { process_word.data() };
    char *c_args[] = {c_script, c_command, c_to_complete, c_word, nullptr}; 
    int fds[2]; // Initialize pipes
    pid_t pid { -1 };

    // Fork failure
    if (pipe(fds) == -1) {
        std::cerr << "Error: pipe() failed - " << strerror(errno) << "\n";
        _exit(1);
    }

    // Begin fork
    if ((pid = fork()) ==  -1) {
        std::cerr << "Error: fork() failed - " << strerror(errno) << "\n";
        _exit(1);
    }

    // Child process that begins executable
    else if (pid == 0) {
        // Close unused read pipe
        close(fds[0]);

        // Redirect stdout
        dup2(fds[1], STDOUT_FILENO);

        // Execute external command
        execvp(c_script, c_args);

        // Error if execvp did not execute correctly
        std::cerr << "Error: execvp() failed (1) - " << strerror(errno) << "\n";
        _exit(1);
    }  

    else {
        // Close unused write pipe
        close(fds[1]);

        // Wait for child process to terminate
        int status;
        pid_t waited_pid { waitpid(pid, &status, 0) };
        if (waited_pid == -1) {
            std::cerr << "Error: waitpid() failed - " << strerror(errno) << "\n";
        }

        // Read all stddout of script from pipe 
        int bytes;
        const int MSGSIZE { 16 };
        char buffer[MSGSIZE];
        std::string outputs { "" };
        while ((bytes = read(fds[0], buffer, MSGSIZE)) > 0) {
            int i { 0 };
            while (i < MSGSIZE && buffer[i] != '\0') {
                outputs += buffer[i];
                buffer[i] = '\0';
                i++;
            }
        }

        // Parse multiple candidate matches
        std::stringstream ss { parse_quotes(outputs) };
        std::vector<std::string> all_matches {};
        std::string output {};
        int min_size { INT_MAX };
        while (std::getline(ss, output, ss_separator)) {
            trim(output, true);
            // Ignore word if it has invalid characters or does not match word to be completed
            bool ignore_word { false };
            for (int i = 0; i < output.size(); i++) {
                if ((i < to_complete.size() && to_complete[i] != output[i]) || (!std::isalnum(output[i]) && output[i] != '-' && output[i] != '_')) {
                    ignore_word = true;
                    break;
                }
            }
            if (!ignore_word) {
                int size = output.size();
                min_size = std::min(min_size, size);
                all_matches.push_back(output);
            }
        }

        // Add to line buffer
        std::string completed_str = command + ' ';
        if (!word.empty()) {
            completed_str += word + ' ';
        }

        // Restore incomplete string if no match could be made
        if (all_matches.size() <= 0) {
            completed_str += to_complete;
            std::cout << '\x07'; // Ring bell
            double_tab = false;
        }

        // There is exactly one match to complete
        else if (all_matches.size() == 1) {
            completed_str += all_matches[0] + ' ';
            double_tab = false;
        }

        // There are multiple matches to display
        else {
            completed_str += to_complete;

            // Compare character by character to find the longest common prefix
            int i = to_complete.size();
            std::string to_add { "" };
            while (i < min_size) {
                char character { all_matches[0][i] };
                bool mismatch { false };
                for (int j = 1; j < all_matches.size(); j++) {
                    if (character != all_matches[j][i]) {
                        mismatch = true;
                        break;
                    }
                }
                if (mismatch) { break; }
                to_add += character;
                i++;
            }
            // Complete to longest common prefix so long as string is not empty
            if (!to_add.empty()) {
                completed_str += to_add;
                char *completed { completed_str.data() };
                add_to_rl_line_buffer(completed, strlen(completed));
                double_tab = false;
            }
            // Ring tab if no progress is made
            else if (!double_tab) {
                std::cout << '\x07'; // Ring bell
                double_tab = true;
            } 
            // The second tab prints all matches
            else {
                std::cout << '\n';
                std::sort(all_matches.begin(), all_matches.end());
                for (int i = 0; i < all_matches.size(); i++) {
                    std::cout << all_matches[i];
                    // Print spaces in between matches
                    if (i < all_matches.size() - 1) {
                        std::cout << "  ";
                    } 
                }
                std::cout << '\n';
                rl_forced_update_display();
                double_tab = false;
            }
        }

        char *completed { completed_str.data() };
        add_to_rl_line_buffer(completed, strlen(completed));
    }
}

// Autocomplete files and directories
void autocomplete_files_and_dirs(int start) {
    std::vector<std::string> all_matches {};
    std::string full_string { rl_line_buffer };
    std::string completed_str { full_string.substr(0, start) };
    std::string to_match = full_string.substr(start, full_string.size() - start);
    std::string path { std::filesystem::current_path() };
    std::string nested_dir { "" };
    // Determine the last time a backslash appears
    int last_slash { -1 };
    for (int i = to_match.size() - 1; i >= 0; i--) {
        if (to_match[i] == '/') {
            last_slash = i;
            break;
        }
    }
    // Search nested path if given by user
    if (last_slash > -1) {
        nested_dir = to_match.substr(0, last_slash + 1);
        path += '/' + nested_dir;
        completed_str += nested_dir;
        // Determine if there is a file to match or blank space
        int file_start = last_slash + 1;
        if (file_start == to_match.size()) {
            to_match = "";
        }
        else {
            to_match = to_match.substr(file_start, to_match.size() - file_start);
        }
    }
    // Ensure directory exists before searching for files
    int min_size { INT_MAX };
    if (std::filesystem::is_directory(path)) {
        for (auto f : std::filesystem::directory_iterator(path)) {
            std::string file { f.path() };
            int size { add_match(file, to_match, all_matches) };
            min_size = std::min(min_size, size);
        }
    }
    else {
        min_size = 0;
    }

    // No matches were found
    if (all_matches.size() == 0) {
        std::cout << '\x07'; // Ring bell when autocomplete is not possible
        double_tab = false;
    }
    // Exactly one match was found
    else if (all_matches.size() == 1) {
        completed_str += all_matches[0];
        // Complete with slash for directories and space for files
        if (std::filesystem::is_directory(path + '/' + all_matches[0])) {
            completed_str += '/';
        } else {
            completed_str += ' ';
        }
        char *completed { completed_str.data() };
        add_to_rl_line_buffer(completed, strlen(completed));
        double_tab = false;
    }
    // Multiple matches were found
    else {
        // Compare character by character to find the longest common prefix
        int i = to_match.size();
        std::string to_add { to_match };
        while (i < min_size) {
            char character { all_matches[0][i] };
            bool mismatch { false };
            for (int j = 1; j < all_matches.size(); j++) {
                if (character != all_matches[j][i]) {
                    mismatch = true;
                    break;
                }
            }
            if (mismatch) { break; }
            to_add += character;
        i++;
        }
        // Autocomplete if the terminal string will be longer
        if (to_add.size() > to_match.size()) {
            completed_str += to_add;
            char *completed { completed_str.data() };
            add_to_rl_line_buffer(completed, strlen(completed));
            double_tab = false;
        }
        else {
            // The first tab only rings the bell
            if (!double_tab) {
                std::cout << '\x07'; // Ring bell
                double_tab = true;
            } 
            // The second tab prints all matches
            else {
                std::cout << '\n';
                std::sort(all_matches.begin(), all_matches.end());
                for (int i = 0; i < all_matches.size(); i++) {
                    std::cout << all_matches[i];
                    // Print slashes after directories
                    if (std::filesystem::is_directory(path + '/' + all_matches[i])) {
                        std::cout << '/';
                    }
                    // Print spaces in between matches
                    if (i < all_matches.size() - 1) {
                        std::cout << "  ";
                    } 
                }
                std::cout << '\n';
                rl_forced_update_display();
                double_tab = false;
            }
        }
    }
}

// Autocomplete executable command
void autocomplete_executables() {
    // Collect all executables that match current content
    std::string all_paths { load_paths() }; 
    std::stringstream ss(all_paths);
    std::string path;
    std::vector<std::string> all_matches {};
    std::string to_match { rl_line_buffer };
    int min_size { INT_MAX }; // Minimum size of all matching executables
    while (std::getline(ss, path, os_separator)) {
    // Get all executables from a directory
        if (std::filesystem::is_directory(path)) {
            try {
                for (auto fp : std::filesystem::directory_iterator(path)) {
                    std::string file_path { fp.path() };
                    if (executable_exists_with_perms(file_path)) {
                        int size { add_match(file_path, to_match, all_matches) };
                        min_size = std::min(min_size, size);
                    }
                }
            } catch(std::filesystem::filesystem_error &error) {}
        } 
        // Add executable to path if it exists and has execute permissions
        else {
            if (executable_exists_with_perms(path)) {
                int size { add_match(path, to_match, all_matches) };
                min_size = std::min(min_size, size);
            }
        }
    }
    // No matches were found
    if (all_matches.size() == 0) {
        std::cout << '\x07'; // Ring bell when autocomplete is not possible
        double_tab = false;
    }
    // Exactly one match was found
    else if (all_matches.size() == 1) {
        all_matches[0] += ' ';
        char *completed { all_matches[0].data() };
        add_to_rl_line_buffer(completed, strlen(completed));
        double_tab = false;
    }
    // Multiple matches were found
    else {
        // Compare character by character to find the longest common prefix
        int i = to_match.size();
        std::string to_add { "" };
        while (i < min_size) {
            char character { all_matches[0][i] };
            bool mismatch { false };
            for (int j = 1; j < all_matches.size(); j++) {
                if (character != all_matches[j][i]) {
                    mismatch = true;
                    break;
                }
            }
            if (mismatch) { break; }
            to_add += character;
            i++;
        }
        // Autocomplete so long as string is not empty
        if (!to_add.empty()) {
            to_match += to_add;
            char *completed { to_match.data() };
            add_to_rl_line_buffer(completed, strlen(completed));
            double_tab = false;
        }
        else {
            // The first tab only rings the bell
            if (!double_tab) {
                std::cout << '\x07'; // Ring bell
                double_tab = true;
            } 
            // The second tab prints all matches
            else {
                std::cout << '\n';
                std::sort(all_matches.begin(), all_matches.end());
                for (int i = 0; i < all_matches.size(); i++) {
                    std::cout << all_matches[i];
                    if (i < all_matches.size() - 1) {
                        std::cout << "  ";
                    } 
                }
                std::cout << '\n';
                rl_forced_update_display();
                double_tab = false;
            }
        }
    }
}

// Determine if given path exists and is executable
bool executable_exists_with_perms(std::string &path) {
    std::filesystem::path p { path };
    std::filesystem::file_status s { status(p) };
    std::filesystem::perms file_perms { s.permissions() };
    return std::filesystem::exists(s) && (file_perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
}

// Tracks executable/file if it matches current keyboard state (and is not already included)
int add_match(
    std::string &path,
    std::string &to_match, 
    std::vector<std::string> &all_matches
) {
    size_t i { path.find_last_of('/') + 1};
    std::string file { path.substr(i, path.size() - i) };
    int count = std::count(all_matches.begin(), all_matches.end(), file);
    if (count == 0 && file.substr(0, to_match.size()).compare(to_match) == 0) {
        all_matches.push_back(file);
        return file.size();
    }
    return INT_MAX;
}






/*

Function definitions for jobs command

*/

// If a command ends with '&', run it in the background
void run_background_job(std::string line) {
    std::stringstream ss { parse_quotes(line.substr(0, line.size() - 1)) };
    std::string output;
    std::vector<std::string> command_line {};
    while (std::getline(ss, output, ss_separator)) {
        if (!output.empty()) {
            command_line.push_back(output);
        }
    }

    // Convert strings into c-style strings
    char *c_command_line[20] { nullptr };
    int i { 0 };
    for (; i < command_line.size(); i++) {
        c_command_line[i] = command_line[i].data();
    }
    c_command_line[i] = nullptr;

    // Create fork that execvp use to create a background process
    pid_t pid { fork() };
    
    // Fork failure
    if (pid == -1) {
        std::cerr << "Error: fork() failed - " << strerror(errno) << "\n";
        exit(1);
    }

    // Child process that begins background process
    else if (pid == 0) {
        execvp(c_command_line[0], c_command_line);
        
        std::cerr << "Error: execvp() failed (3) - " << strerror(errno) << "\n";
        exit(1);
    }

    // Print job id and pid
    else {
        // Either recycle numbers of finished jobs or assign a new higher id
        int job_id;
        if (!done_job_ids.empty()) {
            job_id = *done_job_ids.begin();
            done_job_ids.erase(job_id);
        }
        else {
            job_id = job_map.size() + 1;
            highest_job = job_id;
        }
    job_map[job_id] = make_pair(pid, line); // Save job in map
    std::cout << "[" << job_id << "] " << pid << "\n";
    }
}

// Add all finished background jobs to separate set for tracking
void find_exited_jobs() {
    pid_t pid;
    // int status;
    int job_id { 0 };
    std::pair<pid_t, std::string> job_pair;

    while (job_id < highest_job) {
        job_id++;

        // Skip job if not in map
        if (!job_map.contains(job_id)) {
            continue;
        }

        // Check if background process ended
        job_pair = job_map[job_id];
        pid = std::get<0>(job_pair);
        int status { 0 };
        pid_t result { waitpid(pid, &status, WNOHANG) }; // Check without waiting to finish
        if (result == pid && WIFEXITED(status)) {
            done_job_ids.insert(job_id);
        }
    }
}

// Display background jobs; either all for "jobs" command or just completed ones
void display_jobs(
    bool show_all
) {
    // Iterate over each background job and print status
    int job_id { 0 };
    int active_jobs { 0 }; // Counter for the number of active jobs processed so far
    std::string command;
    std::string state;
    char marker;
    std::pair<pid_t, std::string> job_pair;
    while (job_id < highest_job) {
        job_id++;

        // Skip job if not in map
        if (!job_map.contains(job_id)) {
            continue;
        }
        active_jobs++;

        // Avoid printing currently running jobs for reap before the next prompt 
        if (!show_all && !done_job_ids.contains(job_id)) {
            continue;
        }

        // Load and determine pair details
        job_pair = job_map[job_id];
        command = std::get<1>(job_pair);

        if (done_job_ids.contains(job_id)) {
            state = "Done                 ";
            command = command.substr(0, command.size() - 2); // Remove ending " &" of all background commands
        } else {
            state = "Running                 ";
        }

        // Determine marker symbol to display for jobs
        if (active_jobs == job_map.size()) { // Most recent job has '+'
            marker = '+';
        } else if (job_id + 1 == job_map.size()) { // Second most recent has '-'
            marker = '-';
        } else {
            marker = ' ';
        }
        std::cout << "[" << job_id << "]" << marker << "  " << state << command << '\n';
    }
    // Reap finished jobs
    for (int job_id: done_job_ids) {
        job_map.erase(job_id);
    }
    done_job_ids.clear();
}






/*

Function definitions for pipelining

*/

// Function to start the entire pipeline of commands
void start_pipeline(
    int pipe_start,
    std::string &line, 
    std::vector<std::string> pipeline_commands
) {
    // Add final command to pipe vector
    if (pipe_start < line.size()) {
        pipeline_commands.push_back(line.substr(pipe_start));
    }

    // Initialize pipes
    int fds[2]; 
    if (pipe(fds) == -1) {
        std::cerr << "Error: pipe() failed - " << strerror(errno) << "\n";
        _exit(1);
    }
    
    // Create fork for first shell process
    pid_t pid { fork() };

    if (pid == -1) {
        std::cerr << "Error: fork() failed - " << strerror(errno) << "\n";
        _exit(1);
    }

    // Set up first child process 
    else if (pid == 0) {
        // Redirect stdout    
        run_pipeline_command(fds, pipeline_commands.size() - 1, pipeline_commands);
    }

    else {
        // Close read (shell does not need to read)
        close(fds[0]);

        // Wait for child process to terminate
        int status;
        waitpid(pid, &status, 0);
    }
}

// Run one pipeline command
void run_pipeline_command(
    int *fds_p,
    int position,
    std::vector<std::string> &pipeline_commands
) {
    // Redirect output to be input for next command
    if (position < pipeline_commands.size() - 1) {
        dup2(fds_p[1], STDOUT_FILENO);
        std::cout << std::flush;
    }

    // Close read end of pipe
    close(fds_p[0]);

    // Set up additional pipes to send stddout if there are more commands to process
    if (position - 1 >= 0) {
        // Initialize next set of pipes
        int fds_c[2];
        if (pipe(fds_c) == -1) {
            std::cerr << "Error: pipe() failed - " << strerror(errno) << "\n";
            _exit(1);
        } 

        // Create next fork
        pid_t pid { fork() };

        if (pid == -1) {
            std::cerr << "Error: fork() failed - " << strerror(errno) << "\n";
            _exit(1);
        }

        // Set up next child process 
        else if (pid == 0) {
            run_pipeline_command(fds_c, position - 1, pipeline_commands);
        }

        else {
            // Close read pipe
            close(fds_c[1]);

            // Redirect this process's output to be input of the next command
            dup2(fds_c[0], STDIN_FILENO);

            // Wait for child process to terminate
            // int status;
            // waitpid(pid, &status, 0);
        }
    }

    // Load command to execute as vector
    std::string line = pipeline_commands[position];
   
    // Match builtins
    trim(line, false);
    if (line.substr(0, 4) == "exit") {
        write_history_on_exit();
        exit(0);
    } else if (line.substr(0, 5) == "echo ") {
        echo(line.substr(5));
        exit(0);
    } else if (line.substr(0, 5) == "type ") {
        type(line.substr(5));
        exit(0);
    } else if (line == "pwd") {
        pwd();
        exit(0);
    } else if (line.substr(0, 3) == "cd ") {
        cd(line.substr(3));
        exit(0);
    } else if (line.substr(0, 9) == "complete ") {
        complete(line.substr(9));
        exit(0);
    } else if (line.substr(0, 4) == "jobs") {
        find_exited_jobs();
        display_jobs(true);
        exit(0);
    }

    // If a builtin was not found, trust that the executable is here
    std::stringstream ss { parse_quotes(line) };
    std::string output;
    std::vector<std::string> command_line {};
    while (std::getline(ss, output, ss_separator)) {
        if (!output.empty()) {
            command_line.push_back(output);
        }
    } 

    // Convert command into c-style array of strings
    char *c_command_line[20] { nullptr };
    int i { 0 };
    for (; i < command_line.size(); i++) {
        c_command_line[i] = command_line[i].data();
    }
    c_command_line[i] = nullptr;

    // Execute c-style command
    execvp(c_command_line[0], c_command_line);
            
    std::cerr << "Error: execvp() failed (2) - " << strerror(errno) << "\n";
    _exit(1);
}






/*

Function definitions for history command

*/

// Display all previously entered commands
void history(std::string &content) {
    std::stringstream ss { parse_quotes(content) };
    std::string output;
    int n = history_vector.size();
    std::string read_file_name { "" };
    std::string write_file_name { "" };
    std::string append_file_name { "" };
    while (std::getline(ss, output, ss_separator)) {
        // -r flag means to look for a file to import history
        if (output == "-r") {
            std::getline(ss, read_file_name, ss_separator);
        } 
        // -w flag to write history to a file 
        else if (output == "-w") {
            std::getline(ss, write_file_name, ss_separator);
        }
        // -a flag to append history to a file
        else if (output == "-a") {
            std::getline(ss, append_file_name, ss_separator);
        }
        // Attempt to load the number of commands to print
        else {
            try { 
                n = std::stoi(content);
            } 
            catch (...) { } 
        }
    }

    // Read history from other file
    if (!read_file_name.empty()) {
        read_history(read_file_name);
    }

    // Write history to an output file
    else if (!write_file_name.empty()) {
        write_history(write_file_name);
    }

    // Append history to an output file
    else if (!append_file_name.empty()) {
        // Open file in append mode
        std::ofstream append_file;
        append_file.open(append_file_name, std::ios_base::app);

        // Abort if file has error being opened
        if (!append_file.is_open()) {
            std::cerr << "Error opening the file " << append_file_name << " for appending!\n";
        }

        // Write fresh append history to file
        for (int i { last_append }; i < history_vector.size(); i++) {
            append_file << history_vector[i] << '\n';
        }
        last_append = history_vector.size();

        // Close file
        append_file.close();
    }

    // Print the last n lines of history
    else {
        for (int i = history_vector.size() - n; i < history_vector.size(); i++) {
            std::cout << "    " << i+1 << ' ' << history_vector[i] << '\n'; 
        }
    }
}

// Read history from file into main memory
void read_history(std::string &read_file_name) {
    // Open persistence file to read
    std::ifstream read_file(read_file_name);

    // Abort if file opened wrong
    if (!read_file.is_open()) {
        std::cerr << "Error opening the file " << read_file_name << " for reading!\n";
        exit(1);
    }

    // Read and add each command to history
    std::string output;
    while (std::getline(read_file, output)) {
        history_vector.push_back(output);
    }

    // Close the file
    read_file.close();
}

// Write history from memory into file
void write_history(std::string &write_file_name) {
    std::ofstream write_file(write_file_name);

    // Abort if write file could not be opened
    if (!write_file.is_open()) {
        std::cout << "Error opening the file " << write_file_name << " for writing!\n";
    }

    // Write all history to output file
    for (int i { 0 }; i < history_vector.size(); i++) {
        write_file << history_vector[i] << '\n';
    }

    write_file.close();
}

// Write history to file after shell exits
void write_history_on_exit() {
    // Load history file environment variable from memory
    char *c_write_file_name { getenv("HISTFILE") }; 
    if (c_write_file_name != nullptr) {
        std::string write_file_name { c_write_file_name }; // Convert to string
        write_history(write_file_name);    
    }
}

// Cycle through previous commands 
int rl_up_arrow(int count, int key) {
    if (history_index > 0) {
        history_index--;
        char *c_command { history_vector[history_index].data() };
        add_to_rl_line_buffer(c_command, strlen(c_command));
    }
    return 0;
}

// Sift through newer commands
int rl_down_arrow(int count, int key) {
    if (history_index < history_vector.size() - 1) {
        history_index++;
        char *c_command { history_vector[history_index].data() };
        add_to_rl_line_buffer(c_command, strlen(c_command));
    } else if (history_index == history_vector.size() - 1) {
        char *c_command { (char *) "" };
        add_to_rl_line_buffer(c_command, strlen(c_command));
    }
    return 0;
}






/*

Function definitions for parameter expansion

*/

// Declare command for adding shell variables
void declare(std::string content) {
    std::stringstream ss { parse_quotes(content) };
    std::string output;
    std::string variable { "" };
    std::string value { "" };
    while (std::getline(ss, output, ss_separator)) {
        // -p flag means to print value if registered
        if (output == "-p") {
            std::getline(ss, variable, ss_separator);
        }
        // Separate variable from value if possible
        else {
            for (int i { 0 }; i < output.size(); i++) {
                if (output[i] == '=' && i + 1 < output.size()) {
                    variable = output.substr(0, i);
                    value = output.substr(i + 1);
                    break;
                }
            }   
        }
    }

    // Register shell variable with value
    if (!variable.empty() && !value.empty()) {
        // Valid the variable, which must start with a letter/underscore and contain only letters/underscores/digits
        bool valid { true };
        for (int i { 0 }; i < variable.size(); i++) {
            if (i == 0 && !isalpha(variable[i]) && variable[i] != '_') {
                valid = false;
                break;
            } else if (!isalnum(variable[i]) && variable[i] != '_') {
                valid = false;
                break;
            }
        }

        // Save value with variable
        if (valid) {
           declare_map[variable] = value;
        } else {
            std::cout << "declare: `" << variable << '=' << value << "': not a valid identifier\n";
        }
    }
    
    // Print variable value if registered
    else if (!variable.empty()) {
        if (declare_map.find(variable) != declare_map.end()) {
            std::cout << "declare -- " << variable << "=\"" << declare_map[variable] << "\"\n";
        } else {
            std::cout << "declare: " << variable << ": not found\n";
        }
    }
}

// Expand command with declared variables
std::string expand(std::string &line) {
    int i { 0 };
    std::string variable { "" };
    std::string expanded { "" };
    while (i < line.size()) {
        // '$' indicates the start of expansion
        if (line[i] == '$') {
            i++;
            // Expand variables encase in brackets
            if (line[i] == '{') { i++; }

            // Build variable name until non-alphanumeric, non-underscore character is found
            while (isalnum(line[i]) || line[i] == '_') {
                variable += line[i];
                i++;
            }

            // Process brackets fully
            if (line[i] == '}') { i++; }

            // Add variable to command if it is registered
            if (declare_map.find(variable) != declare_map.end()) {
                expanded += declare_map[variable];
            }

            // Reset variable
            variable = "";
        }

        else {
            expanded += line[i];
            i++;
        }

    }
    return expanded;
}
