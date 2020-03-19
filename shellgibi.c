#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>

// ansi color codes
// TODO: sahbaz https://bluesock.org/~willkg/dev/ansi.html
#define ANSI_COLOR_RESET "\x1b[0m"
#define ANSI_COLOR_ERROR "\x1b[1;31m"
#define ANSI_COLOR_WARNING "\x1b[1;33;46m"

const char *sysname = "shellgibi";

enum return_codes
{
    SUCCESS = 0,
    EXIT = 1,
    UNKNOWN = 2,
    INVALID = 3
};

struct command_t
{
    char *name;
    bool background;
    int arg_count;
    char **args;
    char *redirects[3];     // in/out redirection
    struct command_t *next; // for piping
};

struct autocomplete_match
{
    int match_count;
    char **matches;
};

char **all_available_commands;
int number_of_available_commands;

char *shellgibi_builtin_commands[] = {"myjobs", "pause", "mybg", "myfg", "alarm", "psvis", "corona", "hwtim"};

struct autocomplete_match *shellgibi_autocomplete(const char *input_str);

struct autocomplete_match *filename_autocomplete(const char *input_str);

char *get_command_name(char *buf);

void print_warning(char *message);

void print_error(char *message);

void combine_path(char *, char *, char *);

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command)
{
    int i = 0;
    printf("Command: <%s>\n", command->name);
    printf("\tIs Background: %s\n", command->background ? "yes" : "no");
    printf("\tRedirects:\n");
    for (i = 0; i < 3; i++)
        printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
    printf("\tArguments (%d):\n", command->arg_count);
    for (i = 0; i < command->arg_count; ++i)
        printf("\t\tArg %d: %s\n", i, command->args[i]);
    if (command->next)
    {
        printf("\tPiped to:\n");
        print_command(command->next);
    }
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
    if (command->next)
    {
        free_command(command->next);
        command->next = NULL;
    }
    if (command->arg_count)
    {
        for (int i = 0; i < command->arg_count; ++i)
        {
            free(command->args[i]);
            command->args[i] = NULL;
        }
        free(command->args);
        command->args = 0;
    }
    for (int i = 0; i < 3; ++i)
        if (command->redirects[i])
        {
            free(command->redirects[i]);
            command->redirects[i] = NULL;
        }
    free(command->name);
    command->name = NULL;
    free(command);
    return 0;
}

int free_autocomplete_match(struct autocomplete_match *match)
{
    if (match->match_count)
    {
        for (int i = 0; i < match->match_count; ++i)
        {
            free(match->matches[i]);
            match->matches[i] = NULL;
        }
        free(match->matches);
        match->match_count = 0;
    }
    return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
    char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
    getcwd(cwd, sizeof(cwd));
    //    printf("Mypid is %d\n", getpid());
    printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
    return 0;
}

char *get_command_name(char *buf)
{
    const char *splitters = " \t"; // split at whitespace
    int len = strlen(buf);
    while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
        buf[--len] = 0; // trim right whitespace

    char *pch = strtok(buf, splitters);

    if (pch == NULL)
        return NULL;
    else
        return strdup(pch);
}

int should_complete_filename(char *buf, char *filename_start)
{
    const char *splitters = " \t"; // split at whitespace
    int len;
    len = strlen(buf);
    while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    int have_spaces_at_the_end = 0;
    while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
    {
        buf[--len] = 0; // trim right whitespace
        have_spaces_at_the_end = 1;
    }

    char *pch = strtok(buf, splitters);

    // empty string
    if (pch == NULL)
        return 0;

    // at least the command and a space is entered
    if (have_spaces_at_the_end)
    {
        filename_start[0] = '\0';
        return 1;
    }

    pch = strtok(NULL, splitters);
    // we only have the command part without empty string
    // it could be unfinished
    if (pch == NULL)
        return 0;

    char *last_token = pch;

    while ((pch = strtok(NULL, splitters)) != NULL)
    {
        last_token = pch;
    }

    strcpy(filename_start, last_token);
    return 1;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
    const char *splitters = " \t"; // split at whitespace
    int index, len;
    len = strlen(buf);
    while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
        buf[--len] = 0; // trim right whitespace

    if (len > 0 && buf[len - 1] == '&') // background
        command->background = true;

    char *pch = strtok(buf, splitters);
    command->name = (char *)malloc(strlen(pch) + 1);
    if (pch == NULL)
        command->name[0] = 0;
    else
        strcpy(command->name, pch);

    command->args = (char **)malloc(sizeof(char *));

    int redirect_index;
    int arg_index = 0;
    char temp_buf[1024], *arg;
    while (1)
    {
        // tokenize input on splitters
        pch = strtok(NULL, splitters);
        if (!pch)
            break;
        arg = temp_buf;
        strcpy(arg, pch);
        len = strlen(arg);

        if (len == 0)
            continue;                                        // empty arg, go for next
        while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
        {
            arg++;
            len--;
        }
        while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
            arg[--len] = 0; // trim right whitespace
        if (len == 0)
            continue; // empty arg, go for next

        // piping to another command
        if (strcmp(arg, "|") == 0)
        {
            struct command_t *c = malloc(sizeof(struct command_t));
            memset(c, 0, sizeof(struct command_t));
            int l = strlen(pch);
            pch[l] = splitters[0]; // restore strtok termination
            index = 1;
            while (pch[index] == ' ' || pch[index] == '\t')
                index++; // skip whitespaces

            parse_command(pch + index, c);
            pch[l] = 0; // put back strtok termination
            command->next = c;
            continue;
        }

        // background process
        if (strcmp(arg, "&") == 0)
            continue; // handled before

        // handle input redirection
        redirect_index = -1;
        if (arg[0] == '<')
            redirect_index = 0;
        if (arg[0] == '>')
        {
            if (len > 1 && arg[1] == '>')
            {
                redirect_index = 2;
                arg++;
                len--;
            }
            else
                redirect_index = 1;
        }
        if (redirect_index != -1)
        {
            command->redirects[redirect_index] = malloc(len);
            strcpy(command->redirects[redirect_index], arg + 1);
            continue;
        }

        // normal arguments
        if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"') || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
        {
            arg[--len] = 0;
            arg++;
        }
        command->args = (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
        command->args[arg_index] = (char *)malloc(len + 1);
        strcpy(command->args[arg_index++], arg);
    }
    command->arg_count = arg_index;
    return 0;
}

void prompt_backspace()
{
    putchar(8);   // go back 1
    putchar(' '); // write empty over
    putchar(8);   // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
    int index = 0;
    char c;
    char buf[4096];
    static char oldbuf[4096];
    char filename_buf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

    //FIXME: backspace is applied before printing chars
    show_prompt();
    int multicode_state = 0;
    buf[0] = 0;
    while (1)
    {
        c = getchar();
        // printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging
        if (c == 9) // handle tab
        {
            if (index == 0)
            {
                continue;
            }
            char *buf_dup = strdup(buf);
            buf_dup[index] = '\0';

            struct autocomplete_match *match;
            int is_filename = should_complete_filename(buf_dup, filename_buf);

            if (is_filename)
            {
                match = filename_autocomplete(filename_buf);
                if (match->match_count == 1)
                {
                    // complete the command
                    int input_filename_len = strlen(filename_buf);
                    int match_len = strlen(match->matches[0]);

                    if (match_len != input_filename_len)
                    {
                        for (int i = input_filename_len; i < match_len; i++)
                        {
                            putchar(match->matches[0][i]); // echo the character
                            buf[index++] = match->matches[0][i];
                        }
                    }
                    c = ' ';
                }
                else if (match->match_count > 1)
                {
                    printf("\n");
                    for (int i = 0; i < match->match_count; i++)
                    {
                        printf("%s\t", match->matches[i]);
                    }
                    printf("\n");
                    show_prompt();
                    printf("%s", buf);
                }
            }
            else
            {
                // auto complete command
                buf_dup = strdup(buf);
                buf_dup[index] = '\0';
                char *command_name = get_command_name(buf_dup);
                match = shellgibi_autocomplete(command_name);
                if (match->match_count == 1)
                {
                    // complete the command
                    int input_command_len = strlen(command_name);
                    int match_len = strlen(match->matches[0]);

                    if (match_len != input_command_len)
                    {
                        for (int i = input_command_len; i < match_len; i++)
                        {
                            putchar(match->matches[0][i]); // echo the character
                            buf[index++] = match->matches[0][i];
                        }
                    }
                    c = ' ';
                }
                else if (match->match_count > 1)
                {
                    printf("\n");
                    for (int i = 0; i < match->match_count; i++)
                    {
                        printf("%s\t", match->matches[i]);
                    }
                    printf("\n");
                    show_prompt();
                    printf("%s", buf);
                }
            }
            free_autocomplete_match(match);
            if (c == 9)
            {
                continue;
            }
        }

        if (c == 127) // handle backspace
        {
            if (index > 0)
            {
                prompt_backspace();
                index--;
            }
            continue;
        }
        if (c == 27 && multicode_state == 0) // handle multi-code keys
        {
            multicode_state = 1;
            continue;
        }
        if (c == 91 && multicode_state == 1)
        {
            multicode_state = 2;
            continue;
        }
        if (c == 65 && multicode_state == 2) // up arrow
        {
            int i;
            while (index > 0)
            {
                prompt_backspace();
                index--;
            }
            for (i = 0; oldbuf[i]; ++i)
            {
                putchar(oldbuf[i]);
                buf[i] = oldbuf[i];
            }
            index = i;
            multicode_state = 0;
            continue;
        }

        putchar(c); // echo the character
        buf[index++] = c;
        if (index >= sizeof(buf) - 1)
            break;
        if (c == '\n') // enter key
            break;
        if (c == 4) // Ctrl+D
            return EXIT;
    }
    if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
        index--;
    buf[index] = '\0'; // null terminate string

    strcpy(oldbuf, buf);

    parse_command(buf, command);

    // print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
    return SUCCESS;
}

int qstrcmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

void load_all_available_commands()
{
    char *path = strdup(getenv("PATH"));
    int total_number_of_executables = sizeof(shellgibi_builtin_commands) / sizeof(shellgibi_builtin_commands[0]);
    all_available_commands = malloc(total_number_of_executables * sizeof(char *));
    memcpy(all_available_commands, shellgibi_builtin_commands, sizeof(shellgibi_builtin_commands));
    char *file_name_buffer[65536];

    int executables_in_dir;
    char *path_tokenizer = strtok(path, ":");
    while (path_tokenizer != NULL)
    {
        DIR *directory;
        struct dirent *directory_entry;
        executables_in_dir = 0;
        directory = opendir(path_tokenizer);
        if (directory)
        {
            while ((directory_entry = readdir(directory)) != NULL)
            {
                if (directory_entry->d_name[0] == '.')
                {
                    continue;
                }
                char full_path[strlen(path_tokenizer) + strlen(directory_entry->d_name) + 1];
                combine_path(full_path, path_tokenizer, directory_entry->d_name);
                if (access(full_path, X_OK) != 0)
                    continue;
                file_name_buffer[executables_in_dir++] = strdup(directory_entry->d_name);
            }
            closedir(directory);

            if (executables_in_dir != 0)
            {
                total_number_of_executables += executables_in_dir;
                all_available_commands = realloc(all_available_commands,
                                                 total_number_of_executables * sizeof(char *));
                for (int i = 0; i < executables_in_dir; i++)
                {
                    all_available_commands[total_number_of_executables - executables_in_dir + i] = file_name_buffer[i];
                }
            }
        }
        path_tokenizer = strtok(NULL, ":");
    }

    qsort(all_available_commands, total_number_of_executables, sizeof(char *), qstrcmp);

    int unique_number_of_executables = 1;
    for (int i = 1; i < total_number_of_executables; i++)
    {
        if (strcmp(all_available_commands[i], all_available_commands[i - 1]) != 0)
        {
            all_available_commands[unique_number_of_executables++] = all_available_commands[i];
        }
    }
    all_available_commands = realloc(all_available_commands, sizeof(char *) * unique_number_of_executables);
    number_of_available_commands = unique_number_of_executables;
}

int process_command(struct command_t *command, int parent_to_child_pipe[2]);

int execute_command(struct command_t *command);

int execv_command(struct command_t *command);

int execvp_command(struct command_t *command);

int process_command_child(struct command_t *command, const int *child_to_parent_pipe);

int main()
{

    load_all_available_commands();
    // ignore signals from childred to prevent orphan processes
    signal(SIGCHLD, SIG_IGN);

    while (1)
    {
        struct command_t *command = malloc(sizeof(struct command_t));
        memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

        int code;
        code = prompt(command);

        if (code == EXIT)
            break;

        code = process_command(command, NULL);
        if (code == EXIT)
            break;

        free_command(command);
    }

    free(all_available_commands);
    printf("\n");
    return 0;
}

struct autocomplete_match *shellgibi_autocomplete(const char *input_str)
{
    int num_matches = 0;
    char *command;
    struct autocomplete_match *match = malloc(sizeof(struct autocomplete_match));
    memset(match, 0, sizeof(struct autocomplete_match)); // set all bytes to 0

    for (int i = 0; i < number_of_available_commands; i++)
    {
        command = all_available_commands[i];
        if (strncmp(command, input_str, strlen(input_str)) == 0)
        {
            if (num_matches == 0)
            {
                match->matches = (char **)malloc(sizeof(char *));
            }
            else
            {
                match->matches = (char **)realloc(match->matches, sizeof(char *) * (num_matches + 1));
            }
            match->matches[num_matches] = (char *)malloc(strlen(command) + 1);
            strcpy(match->matches[num_matches++], command);
        }
    }

    match->match_count = num_matches;
    return match;
}

struct autocomplete_match *filename_autocomplete(const char *input_str)
{
    int num_matches = 0;
    struct autocomplete_match *match = malloc(sizeof(struct autocomplete_match));
    memset(match, 0, sizeof(struct autocomplete_match)); // set all bytes to 0

    DIR *directory = opendir(".");
    struct dirent *directory_entry;
    if (directory)
    {
        while ((directory_entry = readdir(directory)) != NULL)
        {
            if (directory_entry->d_name[0] == '.')
            {
                continue;
            }
            if (strncmp(directory_entry->d_name, input_str, strlen(input_str)) == 0)
            {
                if (num_matches == 0)
                {
                    match->matches = (char **)malloc(sizeof(char *));
                }
                else
                {
                    match->matches = (char **)realloc(match->matches, sizeof(char *) * (num_matches + 1));
                }
                match->matches[num_matches] = (char *)malloc(strlen(directory_entry->d_name) + 1);
                strcpy(match->matches[num_matches++], directory_entry->d_name);
            }
        }
        closedir(directory);
    }

    match->match_count = num_matches;
    return match;
}

int process_command(struct command_t *command, int parent_to_child_pipe[2])
{

    if (parent_to_child_pipe != NULL)
    {
        close(parent_to_child_pipe[1]);
    }

    int r;
    if (strcmp(command->name, "") == 0)
    {
        if (parent_to_child_pipe != NULL)
        {
            close(parent_to_child_pipe[0]);
        }
        return SUCCESS;
    }

    if (strcmp(command->name, "exit") == 0)
    {
        if (parent_to_child_pipe != NULL)
        {
            close(parent_to_child_pipe[0]);
        }
        return EXIT;
    }

    if (strcmp(command->name, "cd") == 0)
    {
        if (command->arg_count > 0)
        {
            r = chdir(command->args[0]);
            if (r == -1)
                printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
            if (parent_to_child_pipe != NULL)
            {
                close(parent_to_child_pipe[0]);
            }
            return SUCCESS;
        }
    }

    if (strcmp(command->name, "psvis") == 0)
    {
        long root_process = strtol(command->args[0], NULL, 10);

        if (command->arg_count != 2)
        {
            print_error("psvis requires two arguments.");
            return INVALID;
        }

        pid_t pid_s1 = fork();

        if (pid_s1 == 0) //child process
        {
            char temp1[50], temp2[50];
            strcpy(temp1, "PID=");
            sprintf(temp2, "%d", (int)root_process);
            strcat(temp1, temp2);

            command->name = "sudo";
            command->args = (char **)realloc(
                command->args, sizeof(char) * (command->arg_count = 3));

            command->args[0] = "insmod";
            command->args[1] = "psvis.ko";
            command->args[2] = temp1;
            // loading the module
            return execvp_command(command);
        }
        else
        {
            waitpid(pid_s1, NULL, 0); // wait for child process to finish
            pid_t pid_s2 = fork();
            if (pid_s2 == 0)
            { // child process
                command->arg_count = 2;
                command->args = (char **)malloc(command->arg_count * sizeof(char *));
                command->name = "sudo";
                command->args[0] = "rmmod";
                command->args[1] = "psvis";
                // removing the module
                return execvp_command(command);
            }
            else
            {
                waitpid(pid_s2, NULL, 0); // wait for child process to finish
                char *fname = malloc(strlen(command->args[1]) + 1);
                strcpy(fname, command->args[1]);
                command->args = (char **)realloc(
                    command->args, sizeof(char) * (command->arg_count = 2));
                //command->arg_count = 2;
                char *sudo_name = "sudo";
                command->name = realloc(command->name, strlen(sudo_name) + 1);
                strcpy(command->name, sudo_name);
                //command->name = "sudo";
                char *sudo_arg0 = "dmesg";
                char *sudo_arg1 = "-c";
                // in order to direct the output to the file
                command->args[0] = realloc(command->args[0], strlen(sudo_arg0) + 1);
                strcpy(command->args[0], sudo_arg0);
                command->args[1] = realloc(command->args[1], strlen(sudo_arg1) + 1);
                strcpy(command->args[1], sudo_arg1);
                command->redirects[1] = malloc(strlen(fname) + 1);
                strcpy(command->redirects[1], fname);
            }
        }
    }

    // Ahmet Uysal Custom Command, prints the number of coronavirus cases in Turkey
    if (strcmp(command->name, "corona") == 0)
    {
        struct command_t *grep_for_corona_command = malloc(sizeof(struct command_t));
        memset(grep_for_corona_command, 0, sizeof(struct command_t)); // set all bytes to 0
        char temp_filename[16 + 1];
        tmpnam(temp_filename);
        char *grep_name = "grep";
        grep_for_corona_command->name = malloc(strlen(grep_name) + 1);
        strcpy(grep_for_corona_command->name, grep_name);
        grep_for_corona_command->arg_count = 3;
        grep_for_corona_command->args = malloc(grep_for_corona_command->arg_count * sizeof(char *));
        char *grep_arg0 = "-Po";
        grep_for_corona_command->args[0] = malloc(strlen(grep_arg0) + 1);
        strcpy(grep_for_corona_command->args[0], grep_arg0);
        char *grep_arg1 = "<td[^>]*> Turkey </td>(\\s*)<td[^>]*>\\K[0-9]*(?=</td>)";
        grep_for_corona_command->args[1] = malloc(strlen(grep_arg1) + 1);
        strcpy(grep_for_corona_command->args[1], grep_arg1);
        grep_for_corona_command->args[2] = malloc(strlen(temp_filename) + 1);
        strcpy(grep_for_corona_command->args[2], temp_filename);

        // free old args
        for (int i = 0; i < command->arg_count; ++i)
        {
            free(command->args[i]);
        }

        char *wget_name = "wget";
        command->name = realloc(command->name, strlen(wget_name) + 1);
        strcpy(command->name, "wget");
        command->arg_count = 4;
        command->args = realloc(command->args, command->arg_count * sizeof(char *));
        char *wget_arg0 = "--quiet";
        command->args[0] = malloc(strlen(wget_arg0) + 1);
        strcpy(command->args[0], wget_arg0);
        char *wget_arg1 = "--output-document";
        command->args[1] = malloc(strlen(wget_arg1) + 1);
        strcpy(command->args[1], wget_arg1);
        command->args[2] = malloc(strlen(temp_filename) + 1);
        strcpy(command->args[2], temp_filename);
        char *wget_arg3 = "www.worldometers.info/coronavirus/";
        command->args[3] = malloc(strlen(wget_arg3) + 1);
        strcpy(command->args[3], wget_arg3);
        command->next = grep_for_corona_command;
    }

    int child_to_parent_pipe[2];
    int have_child_to_parent_pipe = 0;

    // command is piped to another command, we need to create a pipe to get its stdout and deliver to next command
    if (command->next)
    {
        pipe(child_to_parent_pipe);
        have_child_to_parent_pipe = 1;
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        // child
        if (parent_to_child_pipe != NULL)
        {
            dup2(parent_to_child_pipe[0], STDIN_FILENO);
        }
        return process_command_child(command, child_to_parent_pipe);
    }
    else
    {
        // parent site
        if (parent_to_child_pipe != NULL)
        {
            close(parent_to_child_pipe[0]);
            close(parent_to_child_pipe[1]);
        }

        if (have_child_to_parent_pipe)
        {
            close(child_to_parent_pipe[1]);
        }

        if (!command->background || command->next)
        {
            //            printf("Waiting for child process %d\n", pid);
            waitpid(pid, NULL, 0); // wait for child process to finish
                                   //            printf("Child process finished %s\n", command->name);
        }

        if (strcmp(command->name, "myfg") == 0 && command->arg_count == 1)
        {
            long process_pid = strtol(command->args[0], NULL, 10);
            //            printf("Parent is in myfg %ld\n", process_pid);
            int status;
            while (true)
            {
                status = kill(process_pid, 0);
                if (status == -1 && errno == ESRCH)
                {
                    //                    printf("Child is gone!\n");
                    break;
                }
            }
        }

        if (command->next)
        {
            // how to transfer pipe data?
            int argument_transfer_pipe[2];
            pipe(argument_transfer_pipe);
            char buffer[BUFSIZ];
            ssize_t read_chars;
            while ((read_chars = read(child_to_parent_pipe[0], &buffer, sizeof(buffer))) > 0)
            {
                write(argument_transfer_pipe[1], &buffer, read_chars);
            }
            close(child_to_parent_pipe[0]);
            close(argument_transfer_pipe[1]);
            return process_command(command->next, argument_transfer_pipe);
        }

        return SUCCESS;
    }
}

int process_command_child(struct command_t *command, const int *child_to_parent_pipe)
{
    // Handle redirecting
    int num_redirects_from_stdout = 0;
    if (command->redirects[1] != NULL)
    {
        num_redirects_from_stdout++;
    }
    if (command->redirects[2] != NULL)
    {
        num_redirects_from_stdout++;
    }
    if (command->next)
    {
        num_redirects_from_stdout++;
    }
    // if stdout is redirected to more than one file/pipe, store it in a temp file and copy later
    int stdout_redirected_to_multiple_files = num_redirects_from_stdout > 1;

    // <: input is read from a file
    if (command->redirects[0] != NULL)
    {
        freopen(command->redirects[0], "r", stdin);
    }

    // >: output is written to a file, in write mode
    if (!stdout_redirected_to_multiple_files && command->redirects[1] != NULL)
    {
        freopen(command->redirects[1], "w", stdout);
        freopen(command->redirects[1], "w", stderr);
    }
    // >>: output is written to a file, in append mode
    else if (!stdout_redirected_to_multiple_files && command->redirects[2] != NULL)
    {
        freopen(command->redirects[2], "a", stdout);
        freopen(command->redirects[2], "a", stderr);
    }
    else if (!stdout_redirected_to_multiple_files && command->next)
    {
        dup2(child_to_parent_pipe[1], STDOUT_FILENO);
        dup2(child_to_parent_pipe[1], STDERR_FILENO);
        close(child_to_parent_pipe[1]);
        return execute_command(command);
    }
    else if (stdout_redirected_to_multiple_files)
    {
        char temp_filename[16 + 1];
        tmpnam(temp_filename);
        pid_t pid2 = fork();
        if (pid2 == 0)
        {
            // grandchild
            FILE *tmp_file = fopen(temp_filename, "w");
            dup2(fileno(tmp_file), STDOUT_FILENO);
            return execute_command(command);
        }
        else
        {
            // wait for grandchild to finish and copy temp file to redirected files
            int status;
            waitpid(pid2, &status, 0);
            printf("In commmand %s, waiting for child is done\n", command->name);
            FILE *temp_fp = fopen(temp_filename, "r");

            FILE *trunc_fp = NULL, *append_fp = NULL;

            if (command->redirects[1] != NULL)
            {
                trunc_fp = fopen(command->redirects[1], "w");
            }

            if (command->redirects[2] != NULL)
            {
                append_fp = fopen(command->redirects[2], "a");
            }

            char buffer[BUFSIZ];
            size_t chars_read = 0;
            while ((chars_read = read(fileno(temp_fp), &buffer, sizeof(buffer))) > 0)
            {
                if (command->redirects[1] != NULL)
                {
                    write(fileno(trunc_fp), &buffer, chars_read);
                }

                if (command->redirects[2] != NULL)
                {
                    write(fileno(append_fp), &buffer, chars_read);
                }

                if (command->next)
                {
                    write(child_to_parent_pipe[1], &buffer, sizeof(chars_read));
                }
            }
            fclose(temp_fp);

            if (command->redirects[1] != NULL)
            {
                fclose(trunc_fp);
            }
            if (command->redirects[2] != NULL)
            {
                fclose(append_fp);
            }

            if (command->next)
            {
                close(child_to_parent_pipe[1]);
            }

            remove(temp_filename);
            return SUCCESS;
        }
        // we don't need to redirect to multiple files
        // directly run the execute_command
    }
    return execute_command(command);
}

// directly executes the given command
int execvp_command(struct command_t *command)
{
    command->args = (char **)realloc(
        command->args, sizeof(char *) * (command->arg_count += 2));
    // shift everything forward by 1
    for (int i = command->arg_count - 2; i > 0; --i)
        command->args[i] = command->args[i - 1];

    // set args[0] as a copy of name
    command->args[0] = strdup(command->name);
    // set args[arg_count-1] (last) to NULL
    command->args[command->arg_count - 1] = NULL;
    execvp(command->name, command->args); // exec+args+path
    fprintf(stderr, ANSI_COLOR_ERROR "Error: In execvp call to command: %s\n" ANSI_COLOR_RESET, command->name);
    _exit(1);
}

// responsible for executing both built-in and external commands
int execute_command(struct command_t *command)
{

    if (strcmp(command->name, "myjobs") == 0)
    {
        // myjobs does not accept any args
        if (command->arg_count != 0)
        {
            print_warning("myjobs does not accept any arguments, arguments are omitted");
        }

        char *current_user = getenv("USER");
        command->name = "ps";
        // add 4 extra arguments for ps -U current_user -o pid,cmd,s
        command->args = (char **)realloc(
            command->args, sizeof(char *) * (command->arg_count = 4));

        command->args[0] = "-U";
        command->args[1] = current_user;
        command->args[2] = "-o";
        command->args[3] = "pid,cmd,s";

        return execvp_command(command);
    }

    if (strcmp(command->name, "pause") == 0)
    {
        if (command->arg_count != 1)
        {
            print_error("pause requires only one argument <PID>");
            exit(INVALID);
        }
        long process_pid = strtol(command->args[0], NULL, 10);
        kill(process_pid, SIGSTOP);
        exit(SUCCESS);
    }

    if (strcmp(command->name, "mybg") == 0)
    {
        if (command->arg_count != 1)
        {
            print_error("mybg requires only one argument <PID>");
            exit(INVALID);
        }
        long process_pid = strtol(command->args[0], NULL, 10);
        kill(process_pid, SIGCONT);
        exit(SUCCESS);
    }

    if (strcmp(command->name, "myfg") == 0)
    {
        if (command->arg_count != 1)
        {
            print_error("myfg requires only one argument <PID>");
            exit(INVALID);
        }

        long process_pid = strtol(command->args[0], NULL, 10);
        kill(process_pid, SIGCONT);
        exit(SUCCESS);
    }

    if (strcmp(command->name, "alarm") == 0)
    {
        if (command->arg_count != 2)
        {
            print_error("alarm requires two arguments <HH.MM> <music_file>");
            exit(INVALID);
        }

        char *hour = strtok(command->args[0], ".");
        if (hour == NULL)
        {
            print_error("invalid hour argument");
            exit(INVALID);
        }

        char *minute = strtok(NULL, ".");

        if (minute == NULL)
        {
            print_error("invalid minute argument");
            exit(INVALID);
        }

        FILE *cronjob_file = fopen("new-cronjob.txt", "w");
        fprintf(cronjob_file, "SHELL=/bin/bash\n");
        fprintf(cronjob_file, "PATH=%s\n", getenv("PATH"));
        fprintf(cronjob_file, "%s %s * * * aplay %s\n", minute, hour, command->args[1]);

        fclose(cronjob_file);

        command->name = "crontab";
        command->args = realloc(command->args, (command->arg_count = 1) * sizeof(char *));
        command->args[0] = "new-cronjob.txt";
        return execvp_command(command);
    }

    // Furkan Sahbaz--custom command.
    if (strcmp(command->name, "hwtim") == 0)
    { //handwashing timer
        char *temp1, *temp2;

        if (command->arg_count < 1 || command->arg_count > 2)
        {
            print_error("hwtim requires handwash time and/or email.");
            exit(INVALID);
        }

        if (command->arg_count >= 1)
        {
            int time;
            sscanf(command->args[0], "%d", &time);
            printf("You will be washing your hands for %d seconds.\n", time);
            for (int i = 0; i < time; i++)
            {
                sleep(1);
                printf("%d\n", i + 1);
            }
            printf("You are done washing.\n");
        }

        if (command->arg_count == 2)
        {
            printf("You will  be reminded to wash your hands at 12 am everyday.\n");
            FILE *cronjob_hw_file = fopen("hw-cronjob.txt", "w");
            fprintf(cronjob_hw_file, "SHELL=/bin/bash\n");
            fprintf(cronjob_hw_file, "PATH=%s\n", getenv("PATH"));
            temp1 = "Hand wash reminder!";
            temp2 = "It's time to wash your hands again (use hwtim 20)!";
            fprintf(cronjob_hw_file, "00 12 * * * echo \"%s\" | mail -s  \"%s\" %s \n", temp1, temp2, command->args[1]);

            fclose(cronjob_hw_file);

            command->name = "crontab";
            command->args[0] = "hw-cronjob.txt";
            return execvp_command(command);
        }
    }

    return execv_command(command);
}

// responsible for executing external commands
int execv_command(struct command_t *command)
{
    // increase args size by 2
    command->args = (char **)realloc(
        command->args, sizeof(char *) * (command->arg_count += 2));

    // shift everything forward by 1
    for (int i = command->arg_count - 2; i > 0; --i)
        command->args[i] = command->args[i - 1];

    // set args[0] as a copy of name
    command->args[0] = strdup(command->name);
    // set args[arg_count-1] (last) to NULL
    command->args[command->arg_count - 1] = NULL;
    char *path = getenv("PATH");
    char *path_tokenizer = strtok(path, ":");
    while (path_tokenizer != NULL)
    {
        char full_path[strlen(path_tokenizer) + strlen(command->name) + 1];
        combine_path(full_path, path_tokenizer, command->name);
        execv(full_path, command->args);
        path_tokenizer = strtok(NULL, ":");
    }

    // If we reach here, we couldn't find the command on path
    printf("-%s: %s: command not found\n", sysname, command->name);
    exit(UNKNOWN);
}

void print_warning(char *message)
{
    fprintf(stderr, ANSI_COLOR_WARNING "Warning: %s" ANSI_COLOR_RESET, message);
    fprintf(stderr, "\n");
}

void print_error(char *message)
{
    fprintf(stderr, ANSI_COLOR_ERROR "Error: %s" ANSI_COLOR_RESET, message);
    fprintf(stderr, "\n");
}

void combine_path(char *result, char *directory, char *file)
{
    if ((directory == NULL || strlen(directory) == 0) && (file == NULL || strlen(file) == 0))
    {
        strcpy(result, "");
    }
    else if (file == NULL || strlen(file) == 0)
    {
        strcpy(result, directory);
    }
    else if (directory == NULL || strlen(directory) == 0)
    {
        strcpy(result, file);
    }
    else
    {
        char directory_separator[] = "/";
        const char *directory_last_character = directory;
        while (*(directory_last_character + 1) != '\0')
            directory_last_character++;
        int directory_contains_separator_at_the_end = strcmp(directory_last_character, directory_separator) == 0;
        strcpy(result, directory);
        if (!directory_contains_separator_at_the_end)
            strcat(result, directory_separator);
        strcat(result, file);
    }
}