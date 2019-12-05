/*
 * parse_args.c
 *
 *  Created on: Nov 24, 2019
 *      Author: heath
 */

#include <stdio.h>
#include <getopt.h>

#include "gem_tools.h"
#include "bs_call.h"
#include "bs_call_options.h"

static void usage(const gt_option *const options, char *groups[],
		const bool print_inactive) {
	fprintf(stderr, "USE: ./bs_call [ARGS]...\n");
	gt_options_fprint_menu(stderr, options, groups, true, print_inactive);
}

gt_status parse_arguments(int argc, char **argv, sr_param *const par) {
	gt_status err = GT_STATUS_OK;
	struct option *bs_call_getopt = gt_options_adaptor_getopt(bs_call_options);
	gt_string *const bs_call_short_getopt =  gt_options_adaptor_getopt_short(bs_call_options);
	int option, option_index;
	bool load_seq = false;
	// Additional threads for calculation, input and output
	par->num_threads[CALC_THREADS] = sysconf(_SC_NPROCESSORS_ONLN) - 1;
	while (true) {
		// Get option &  Select case
		if ((option = getopt_long(argc, argv, gt_string_get_string(bs_call_short_getopt),
				bs_call_getopt, &option_index)) == -1)
			break;
		switch (option) {
		/* Operations */
		case '1':
			par->haploid = true;
			break;
		case 'd':
			par->keep_duplicates = true;
			break;
		case 101:
			par->ignore_duplicates = true;
			break;
		case 'k':
			par->keep_unmatched = true;
			break;
		case 'R':
			par->right_trim = atol(optarg);
			break;
		case 'B':
			par->blank_trim = true;
			break;
		case 'L':
			par->left_trim = atol(optarg);
			break;
		case 'q':
			par->mapq_thresh = (uint8_t)atoi(optarg);
			break;
		case 'Q':
			par->min_qual = (uint8_t)atoi(optarg);
			break;
		case 'l':
			par->max_template_len = atol(optarg);
			break;
			/* IO */
		case 'o':
			par->output_file = optarg;
			break;
		case 'n':
			par->sample_name = optarg;
			break;
		case 'r':
			par->name_reference_file = optarg;
			load_seq = 1;
			break;
		case 'O':
			switch(optarg[0]) {
			case 'v':
				par->out_file_type = FT_VCF;
				break;
			case 'z':
				par->out_file_type = FT_VCF_GZ;
				break;
			case 'u':
				par->out_file_type = FT_BCF;
				break;
			case 'b':
				par->out_file_type = FT_BCF_GZ;
				break;
			default:
				gt_fatal_error_msg("File type '%s' not recognized", optarg);
				break;
			}
			break;
		case 'C':
			par->contig_bed = optarg;
			break;
		case 'D':
			par->dbSNP_name = optarg;
			break;
		case 'A':
			par->all_positions = true;
			break;
		case 201:
			par->mmap_input = true;
			break;
		case 202:
			par->report_file = optarg;
			break;
			/* Model */
		case 'c':
			if (sscanf(optarg, "%lf,%lf", &par->under_conv, &par->over_conv) != 2)
				gt_fatal_error_msg(
						"c (conversion) option requires two comma separated arguments");
			if(par->under_conv < 0.0 || par->under_conv > 1.0) par->under_conv = DEFAULT_UNDER_CONVERSION;
			if(par->over_conv < 0.0 || par->over_conv > 1.0) par->over_conv = DEFAULT_OVER_CONVERSION;

			break;
		case 303:
			par->ref_bias = atof(optarg);
			break;
			/* Misc */
		case 'v':
			par->verbose = true;
			break;
		case 't': {
			unsigned int th[3] = {0, 0, 0};
			int rt;
			if((rt = sscanf(optarg, "%u,%d,%u", th, th + 1, th + 2)) == 3) par->explicit_thread_distribution = true;
			else if((rt = sscanf(optarg, "%u", th)) != 1) gt_fatal_error_msg("Could not parse t (threads) option\n");
			for(int k = 0; k < 3; k++) par->num_threads[k] = th[k];
			break;
		}
		case 'h':
		case '?':
			usage(bs_call_options, bs_call_groups, false);
			exit(1);
			break;
		case 'H':
			usage(bs_call_options, bs_call_groups, true);
			exit(1);
			break;
		default:
			usage(bs_call_options, bs_call_groups, false);
			gt_fatal_error_msg("Option '%c' %d not recognized", option, option);
			break;
		}
	}
	if (optind < argc) par->input_file = argv[optind];
	pthread_t dbsnp_thread;
	if(par->dbSNP_name != NULL) {
		pthread_create(&dbsnp_thread, NULL, read_dbSNP_file, par);
	}
	// Sanity
	if (par->min_qual < 1) par->min_qual = 1;
	else if (par->min_qual > MAX_QUAL) par->min_qual = MAX_QUAL;
	// Check input and output files
	if(open_input_file(par) != GT_STATUS_OK) return GT_STATUS_FAIL;

	// Unless explicitly specified, partition the extra threads roughly in proportion 4:3:3 for calc, input and output
	// With a little less going to the input threads depending on how the division works out

	if(!par->explicit_thread_distribution) {
		// We only can use extra input and output threads if the corresponding files are BGZF compressed
		bool input_compressed = par->work.sam_file->format.compression == bgzf;
		bool output_compressed = par->out_file_type & FT_GZ;
		int nn = 10;
		if(!input_compressed) nn -= 3;
		if(!output_compressed) nn -= 3;
		int k = par->num_threads[CALC_THREADS];
		if(input_compressed) {
			par->num_threads[INPUT_THREADS] = k * 3 / nn;
			k -= par->num_threads[INPUT_THREADS];
			nn -= 3;
		} else par->num_threads[INPUT_THREADS] = 0;
		if(output_compressed) {
			par->num_threads[OUTPUT_THREADS] = k * 3 / nn;
			k -= par->num_threads[OUTPUT_THREADS];
		} else par->num_threads[OUTPUT_THREADS] = 0;
		par->num_threads[CALC_THREADS] = k;
	}
	fprintf(stderr, "Additional threads: ");
	for(int i = 0; i < 3; i++) {
		fprintf(stderr," %d", par->num_threads[i]);
	}
	fputc('\n', stderr);
	if (load_seq) {
		fprintf(stderr, "Loading reference sequence index\n");
		bool ret = get_sequence_index(par);
		if(ret) {
			fprintf(stderr, "Could not open index for reference sequence\n");
			err = GT_STATUS_FAIL;
		}
	} else {
		fputs("Error in bs_calls(): a sequence archive is mandatory\n",	stderr);
		err = GT_STATUS_FAIL;
	}
	// Free
	gt_string_delete(bs_call_short_getopt);
	gt_free(bs_call_getopt);
	// JSON Stats
	if(par->report_file != NULL) init_stats(par);
	// dbSNP file
	if(par->dbSNP_name != NULL && err == GT_STATUS_OK) pthread_join(dbsnp_thread, 0);
	return err;
}
