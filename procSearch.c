#include <stdio.h>
#include <stdlib.h>   
#include <unistd.h>   
#include <sys/stat.h>

typedef struct {
    char *target_dir;
    int num_workers;
	char *pattern;
	long min_size;  // -1 for not set
} SearchArguments;

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

int main() {
    
}