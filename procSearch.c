#include <stdio.h>
#include <stdlib.h>   
#include <unistd.h>   
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>

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

int main(int argc, char *argv[]) {
	SearchArguments args;
	get_search_args(argc, argv, &args);

	SubdirPartition  worker_dirs[args.num_workers];
	int final_process_count = distribute_directories(args.target_dir, worker_dirs, args.num_workers);
	if (final_process_count < args.num_workers) {
		args.num_workers = final_process_count;
	}


	for (int i = 0; i < args.num_workers; i++) {
		printf("Worker %d:\n", i);
		for (int j = 0; j < worker_dirs[i].num_dirs; j++) {
			printf("  %s\n", worker_dirs[i].dirs[j]);
		}
	}

	free_worker_dirs(worker_dirs, args.num_workers);

	return 0;
    
}