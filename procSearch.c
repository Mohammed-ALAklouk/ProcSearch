#include <stdio.h>
#include <stdlib.h>   
#include <unistd.h>   
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>



volatile sig_atomic_t workers_finished = 0;
volatile sig_atomic_t sigint_received = 0;
volatile sig_atomic_t sigterm_received = 0;
volatile sig_atomic_t sigterm_sent = 0;

volatile sig_atomic_t unexpected_exit_flag = 0;
volatile sig_atomic_t last_failed_pid = 0;
volatile sig_atomic_t last_failed_status = 0;

volatile sig_atomic_t match_counts[8] = {0};
pid_t worker_pids[8] = {0};

void sigusr1_handler(int sig) {
	(void)sig; // to silence unused parameter warning
    (void)workers_finished;
}

void sigchld_handler(int sig) {
	(void)sig; // to silence unused parameter warning
	int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (int i = 0; i < 8; i++) {
			if (worker_pids[i] == pid) {
				match_counts[i] = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
				break;
			}
		}
		
		if (!sigterm_sent && !WIFEXITED(status)) {  // if we didn't kill them ourselves and they didn't exit normally, flag it as unexpected
			unexpected_exit_flag = 1;
			last_failed_pid = pid;
			last_failed_status = WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status);
		}
		workers_finished++;
    }
}

void sigint_handler(int sig) {
	(void)sig; // to silence unused parameter warning
	sigint_received = 1;
}

void sigterm_handler(int sig) {
	(void)sig; // to silence unused parameter warning
	sigterm_received = 1;
}

void init_signal_handlers() {
	struct sigaction sa;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	
	sa.sa_handler = sigusr1_handler;
	sigaction(SIGUSR1, &sa, NULL);
	
	sa.sa_handler = sigchld_handler;
	sigaction(SIGCHLD, &sa, NULL);

	sa.sa_handler = sigint_handler;
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = sigterm_handler;
	sigaction(SIGTERM, &sa, NULL);
}


typedef struct {
    char *target_dir;
    int num_workers;
	char *pattern;
	long min_size;  // -1 for not set
} SearchArguments;

typedef struct {
	char** dirs;
	int num_dirs;
} SubdirPartition ;

void add_directory(SubdirPartition  *worker_dirs, const char *dir) {
	char** new_dirs = realloc(worker_dirs->dirs, (worker_dirs->num_dirs + 1) * sizeof(char*));
	if(!new_dirs) {
		fprintf(stderr, "Error: Memory allocation failed while adding directory.\n");
		exit(1);
	}

	worker_dirs->dirs = new_dirs;
	worker_dirs->dirs[worker_dirs->num_dirs] = strdup(dir);
	worker_dirs->num_dirs++;
}

void free_worker_dirs(SubdirPartition  *worker_dirs, int num_workers) {
	for (int i = 0; i < num_workers; i++) {
		for (int j = 0; j < worker_dirs[i].num_dirs; j++) {
			free(worker_dirs[i].dirs[j]);
		}
		free(worker_dirs[i].dirs);
	}
}

void print_usage_and_exit() {
	fprintf(stderr, "Usage: ./procSearch -d <root_dir> -n <num_workers> -f <pattern> [-s <min_size_bytes>]\n");
    fprintf(stderr, "  -d <root_dir>        Root directory to search (must exist)\n");
    fprintf(stderr, "  -n <num_workers>     Number of worker processes (2-8 inclusive)\n");
    fprintf(stderr, "  -f <pattern>         Filename pattern (supports + operator)\n");
    fprintf(stderr, "  -s <min_size_bytes>  Optional. Match files with size >= min_size\n");
	exit(1);
}

void get_search_args (int argc, char *argv[], SearchArguments *args) {
	int opt;
    int directory_set = 0;
    int pattern_set = 0; 
    int number_of_workers_set = 0;
    
    args->min_size = -1;

	while ((opt = getopt(argc, argv, "d:n:f:s:")) != -1) {
		long val;
		char* endptr;
        struct stat st;
		switch (opt) {
		case 'd':
			args->target_dir = optarg;
            if (stat(args->target_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "Error: Directory '%s' does not exist.\n", args->target_dir);
                print_usage_and_exit();
            }

			directory_set = 1;
			break;
		case 'n':
            val = strtol(optarg, &endptr, 10);
            if (optarg[0] == '\0' || *endptr != '\0') {
                fprintf(stderr, "Error: Invalid value for -n, must be a number.\n");
                print_usage_and_exit();
            }
            args->num_workers = (int)val;
            if (args->num_workers < 2 || args->num_workers > 8) {
                fprintf(stderr, "Error: Invalid value for -n, must be between 2 and 8.\n");
                print_usage_and_exit();
            }
            number_of_workers_set = 1;
            break;
		case 'f':
			if (optarg[0] == '+') {
				fprintf(stderr, "Error: Invalid regex, '+' cannot be the first character.\n");
				print_usage_and_exit();
			}

			args->pattern = optarg;
			pattern_set = 1;
			break;
		case 's':
			val = strtol(optarg, &endptr, 10);
			if (optarg[0] == '\0' || *endptr != '\0' || val < 0) {
				fprintf(stderr, "Error: Invalid value for -s, must be a positive number.\n");
				print_usage_and_exit();
			}

			args->min_size = val;
			break;

		case '?':
			default:
			fprintf(stderr, "Error: Invalid or missing argument value.\n");
			print_usage_and_exit();
		}
	}
	
	if (!directory_set || !pattern_set || !number_of_workers_set) {
		fprintf(stderr, "Error: Must provide all required arguments.\n");
		print_usage_and_exit(); 
	}
}

int distribute_directories(const char *root_dir, SubdirPartition  *worker_dirs, int num_workers) {
	DIR *dir = opendir(root_dir);
	if (!dir) {
		fprintf(stderr, "Error: Unable to open directory %s\n", root_dir);
		print_usage_and_exit();
	}

	for (int i = 0; i < num_workers; i++) {
		worker_dirs[i].dirs = NULL;
		worker_dirs[i].num_dirs = 0;
	}
	int subdir_count = 0;

	struct dirent* entry;
	
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue; 
		}

		char new_path[4096];
		snprintf(new_path, sizeof(new_path), "%s/%s", root_dir, entry->d_name);

		struct stat statbuf;
		if (lstat(new_path, &statbuf) == -1) continue;
		
		
		if (S_ISDIR(statbuf.st_mode)) {
			add_directory(&worker_dirs[subdir_count % num_workers], new_path);
			subdir_count++;
		}
	}

	if (subdir_count == 0) {
		printf("Notice: no subdirectories found; parent will search root directly.\n");
	} else if (subdir_count < num_workers) {
    	printf("Notice: only %d subdirectories found; using %d workers instead of %d.\n", subdir_count, subdir_count, num_workers);
	}

	closedir(dir);
	return subdir_count;
}

int match_string_ignore_case(const char *a, const char *b) {  
	while (*a && *b) {
		if (*a == '+') {
			char preceding_char = *(a - 1);
			while (tolower(*b) == tolower(preceding_char)) {
				b++;
			}

			a++;
			continue;
		}

		if (tolower(*a) != tolower(*b)) {
			return 0;
		}

		a++;
		b++;
	}

	return *a == '\0' && *b == '\0';
}

int is_match(char* file_name, struct stat statbuf, SearchArguments *criteria) {
	if (S_ISDIR(statbuf.st_mode)) {
		return 0; 
	}

	if (!match_string_ignore_case(criteria->pattern, file_name))  {
		return 0;
	}

	if (criteria->min_size != -1 && statbuf.st_size < criteria->min_size) {
		return 0;
	}

	return 1;
}

int search_root_directory(SearchArguments *criteria) {
	DIR *dir = opendir(criteria->target_dir);
	if (!dir) {
		fprintf(stderr, "Error: Unable to open directory %s\n", criteria->target_dir);
		return 0;
	}

	struct dirent* entry;
	int match_count = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue; // Skip current and parent directory entries
		}

		if (sigterm_received)
			break;
		
		char new_path[4096];
		snprintf(new_path, sizeof(new_path), "%s/%s", criteria->target_dir, entry->d_name);

		struct stat statbuf;
		if (lstat(new_path, &statbuf) == -1) continue;

		if (is_match(entry->d_name, statbuf, criteria)) {
			printf("[Parent] MATCH: %s (%ld bytes)\n", new_path, statbuf.st_size);
			match_count++;
		}
	}

	closedir(dir);
	return match_count;
}

int search_recursive (const char *dir_path, SearchArguments *criteria) {
	if (sigterm_received){
		return 0;
	}
	
	DIR *dir = opendir(dir_path);
	if (!dir) {
		fprintf(stderr, "Error: Unable to open directory %s\n", dir_path);
		return 0;
	}

	struct dirent* entry;
	int match_count = 0;

	while ((entry = readdir(dir)) != NULL) {		
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue; // Skip current and parent directory entries
		}
		
		char new_path[4096];
		snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, entry->d_name);
		
		struct stat statbuf;
		if (lstat(new_path, &statbuf) == -1) continue;
		
		
		if (S_ISDIR(statbuf.st_mode)) {
			match_count += search_recursive (new_path, criteria);
		}
		else if (is_match(entry->d_name, statbuf, criteria)) {
			printf("[Worker PID:%d] MATCH: %s (%ld bytes)\n", getpid(), new_path, statbuf.st_size);
			match_count++;
		}
	}
	closedir(dir);
	return match_count;
}

void print_summary(SearchArguments *args, pid_t *pids, int parent_matches) {
	int total_matches = 0;
	for (int i = 0; i < 8; i++)
		total_matches += match_counts[i];
	
	
	printf("--- Summary ---\n");
	printf("Total workers used  : %d\n", args->num_workers);
	printf("Total matches found : %d\n", total_matches);
	for (int i = 0; i < args->num_workers; i++) {
		printf("Worker PID %d : %d matches\n", pids[i], match_counts[i]);
	}
	printf("Parent process matches : %d\n", parent_matches);
}

void run_workers(SubdirPartition worker_dirs[], SearchArguments args) {
	for (int i = 0; i < args.num_workers; i++)
	{
		worker_pids[i] = fork();
		if (worker_pids[i] < 0) {
			fprintf(stderr, "Error: Failed to fork worker process.\n");
			exit(1);
		} else if (worker_pids[i] == 0) {
			int total_matches = 0;
			for (int j = 0; j < worker_dirs[i].num_dirs; j++) {
				if (sigterm_received) {
					printf("[Worker PID: %d] SIGTERM received. Partial matches: %d. Exiting.\n", getpid(), total_matches);
					exit(total_matches % 256); 
				}

				total_matches += search_recursive(worker_dirs[i].dirs[j], &args);
			}

			printf("[Worker PID: %d] Finished with %d matches. Exiting.\n", getpid(), total_matches);
			kill(getppid(), SIGUSR1);
			exit(total_matches % 256); 
		}
	}
}

void wait_for_workers(SearchArguments args) {
	while (workers_finished < args.num_workers) {
		if (sigint_received) {
			for (int i = 0; i < args.num_workers; i++) {
				if (kill(worker_pids[i], 0) == 0)
					kill(worker_pids[i], SIGTERM);
			}
			sigterm_sent = 1;


			time_t start_time = time(NULL);
			int all_terminated = 0;
			while (time(NULL) - start_time < 3) {
				if (workers_finished == args.num_workers) {
					all_terminated = 1;
					break; 
				}
				
				usleep(100000); // Sleep for just 0.1 seconds, then check again
			}

			if (!all_terminated) {
				printf("[Parent] Forcing termination of all workers...\n");
				for (int i = 0; i < args.num_workers; i++) {
					if (kill(worker_pids[i], 0) == 0)
					{
						kill(worker_pids[i], SIGKILL);
						printf("[Parent] Worker PID: %d did not terminate gracefully, sent SIGKILL.\n", worker_pids[i]);
					}
				}
			}
			break;
		}

		if (unexpected_exit_flag) {
			printf("[Parent] Worker PID: %d terminated unexpectedly (exit status: %d).\n", last_failed_pid, last_failed_status);
			unexpected_exit_flag = 0; 
		}
		pause(); 
	}
}

int main(int argc, char *argv[]) {
	init_signal_handlers();

	SearchArguments args;
	get_search_args(argc, argv, &args);

	SubdirPartition  worker_dirs[args.num_workers];
	int final_process_count = distribute_directories(args.target_dir, worker_dirs, args.num_workers);
	if (final_process_count < args.num_workers) {
		args.num_workers = final_process_count;
	}

	int parent_matches = search_root_directory(&args); 
	
	run_workers(worker_dirs, args);
	wait_for_workers(args);

	print_summary(&args, worker_pids, parent_matches);
	free_worker_dirs(worker_dirs, sizeof(worker_dirs) / sizeof(worker_dirs[0]));
	return 0;
}