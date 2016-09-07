#include <stdio.h>
#include <getopt.h>
#include "args.h"
#include "utils.h"
/* Display usage instructions */
void print_usage(const char *prgname)
{
        PRINT_INFO("\nUsage: %s [EAL options] -- -t TEST_ROLE\n"
                   "  -t TEST_ROLE: [ 0: PROPOSER, 1: COORDINATOR, 2: ACCEPTOR,"
                   "3: LEARNER]", prgname);
}

/* Parse the arguments given in the command line of the application */
void parse_args(int argc, char **argv)
{
        int opt;
        const char *prgname = argv[0];
        /* Disable printing messages within getopt() */
        /* Parse command line */
        while ((opt = getopt(argc, argv, "t:")) != EOF) {
                switch (opt) {
                case 't':
                        client_config.test = atoi(optarg);
                        break;
                default:
                        print_usage(prgname);
                        FATAL_ERROR("Invalid option specified");
                }
        }
        /* Check that options were parsed ok */
        if (client_config.test < PROPOSER || client_config.test > LEARNER) {
                print_usage(prgname);
                FATAL_ERROR("TEST_ROLE is invalid");
        }
}