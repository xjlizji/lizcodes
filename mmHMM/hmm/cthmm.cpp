/*
 *  Continuous-time Variable-duration HMM
 *
 * Copyright (C) 2015-2016 University of Southern California
 *                         Andrew D Smith
 * Author: Andrew D. Smith, Song Qiang
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <numeric>
#include <cmath>
#include <fstream>
#include <iomanip>

#include <unistd.h>

#include "smithlab_utils.hpp"
#include "smithlab_os.hpp"
#include "GenomicRegion.hpp"
#include "OptionParser.hpp"
#include "2DCTHMM.hpp"
#include "distribution.hpp"


using std::string;
using std::vector;
using std::cout;
using std::endl;
using std::cerr;
using std::numeric_limits;
using std::max;
using std::min;
using std::pair;
using std::setw;


static void
load_cpgs(const string &cpgs_file, vector<SimpleGenomicRegion> &cpgs,
          vector<pair<double, double> > &meth, vector<size_t> &reads,
          vector<size_t> &time)
{

  string chrom, prev_chrom;
  size_t pos, prev_pos = 0;
  string strand, seq;
  double level;
  size_t coverage;
  
  std::ifstream in(cpgs_file.c_str());
  while (in >> chrom >> pos >> strand >> seq >> level >> coverage) {
    // sanity check
    if (chrom.empty() || strand.empty() || seq.empty()
        || level < 0.0 || level > 1.0) {
      std::ostringstream oss;
      oss << chrom << "\t" << pos << "\t" << strand << "\t"
      << seq << "\t" << level << "\t" << coverage << "\n";
      throw SMITHLABException("Invalid input line:" + oss.str());
    }
    // order check
    if (prev_chrom > chrom || (prev_chrom == chrom && prev_pos > pos)) {
      throw SMITHLABException("CpGs not sorted in file \"" + cpgs_file + "\"");
    }
    prev_chrom = chrom;
    prev_pos = pos;
    
    // append site
    cpgs.push_back(SimpleGenomicRegion(chrom, pos, pos+1));
    reads.push_back(coverage);
    meth.push_back(std::make_pair(0.0, 0.0));
    meth.back().first = static_cast<size_t>(round(level * coverage));
    meth.back().second = static_cast<size_t>(coverage  - meth.back().first);
    time.push_back(cpgs.back().get_start());
  }
}


template <class T, class U> static void
rm_missingdata(const bool VERBOSE, vector<SimpleGenomicRegion> &cpgs,
               vector<T> &meth, vector<U> &reads, vector<size_t> mytime) {
  if (VERBOSE)
    cerr << "[REMOVE ZERO READ CPGS]" << endl;
  size_t j = 0;
  for (size_t i = 0; i < cpgs.size(); ++i)
    if (reads[i] > 0) {
      cpgs[j] = cpgs[i];
      meth[j] = meth[i];
      reads[j] = reads[i];
      mytime[j] = mytime[i];
      ++j;
    }
  cpgs.erase(cpgs.begin() + j, cpgs.end());
  meth.erase(meth.begin() + j, meth.end());
  reads.erase(reads.begin() + j, reads.end());
  mytime.erase(mytime.begin() + j, mytime.end());

  if (VERBOSE)
    cerr << "CPGS RETAINED: " << cpgs.size() << endl << endl;
}


static void
get_domain_scores(const vector<int> &classes,
                  const vector<pair<double, double> > &meth,
                  vector<double> &scores) {
  static const int CLASS_ID = 1;
  size_t n_cpgs = 0;
  bool in_domain = false;
  double score = 0;
  for (size_t i = 0; i < classes.size(); ++i) {
    if (classes[i] == CLASS_ID) {
      in_domain = true;
      score += 1.0 - (meth[i].first/(meth[i].first + meth[i].second));
      ++n_cpgs;
    }
    else if (in_domain) {
      in_domain = false;
      scores.push_back(score);
      score = 0;
    }
  }
}


static void
shuffle_cpgs(TwoVarHMM &hmm, vector<pair<double, double> > meth,
             const vector<size_t> &mytime, vector<double> &domain_scores) {
  
  srand(time(0) + getpid());
  random_shuffle(meth.begin(), meth.end());
  
  vector<int> classes;
  vector<double> scores;
  hmm.PosteriorDecoding(meth, mytime, classes, scores);
  get_domain_scores(classes, meth, domain_scores);
  sort(domain_scores.begin(), domain_scores.end());
}


static void
assign_p_values(const vector<double> &random_scores,
                const vector<double> &observed_scores,
                vector<double> &p_values) {
  const double n_randoms =
  random_scores.size() == 0 ? 1 : random_scores.size();
  for (size_t i = 0; i < observed_scores.size(); ++i)
    p_values.push_back((random_scores.end() -
                        upper_bound(random_scores.begin(),
                                    random_scores.end(),
                                    observed_scores[i]))/n_randoms);
}


double
get_fdr_cutoff(const vector<double> &scores, const double fdr) {
  if (fdr <= 0)
    return numeric_limits<double>::max();
  else if (fdr > 1)
    return numeric_limits<double>::min();
  vector<double> local(scores);
  std::sort(local.begin(), local.end());
  size_t i = 0;
  for (; i < local.size() - 1 &&
       local[i+1] < fdr*static_cast<double>(i+1)/local.size(); ++i);
  return local[i];
}




static void
build_domains(const bool VERBOSE,
              const vector<SimpleGenomicRegion> &cpgs,
              const vector<double> &post_scores,
              const vector<int> &classes,
              vector<GenomicRegion> &domains) {
  static const int CLASS_ID = 1;
  size_t n_cpgs = 0, n_domains = 0, prev_end = 0;
  bool in_domain = false;
  double score = 0;
  for (size_t i = 0; i < classes.size(); ++i) {
    if (classes[i] == CLASS_ID) {
      if (!in_domain) {
        in_domain = true;
        domains.push_back(GenomicRegion(cpgs[i]));
        domains.back().set_name("HYPO" + toa(n_domains++));
      }
      ++n_cpgs;
      score += post_scores[i];
    }
    else if (in_domain) {
      in_domain = false;
      domains.back().set_end(prev_end);
      domains.back().set_score(n_cpgs);
      n_cpgs = 0;
      score = 0;
    }
    prev_end = cpgs[i].get_end();
  }
}



int
main(int argc, const char **argv) {

  try {
    
    size_t max_iterations = 10;
    
    // run mode flags
    bool VERBOSE = false;
    bool NOFDR = false;
    
    // corrections for small values (not parameters):
    double tolerance = 1e-10;
    double min_prob  = 1e-10;

    string params_in_file, params_out_file;
    string outfile, scores_file;
    
    /****************** COMMAND LINE OPTIONS ********************/
    OptionParser opt_parse(strip_path(argv[0]), "Program for identifying "
			   "HMRs in methylation data", "<cpg-BED-file>");
    opt_parse.add_opt("out", 'o', "output hmr file (default: stdout)",
                      false, outfile);
    opt_parse.add_opt("scores", 's', "scores file (WIG format)",
                      false, scores_file);
    opt_parse.add_opt("itr", 'i', "max iterations", false, max_iterations);
    opt_parse.add_opt("verbose", 'v', "print more run info", false, VERBOSE);
    opt_parse.add_opt("no_fdr_control", 'f', "fdr_control", false, NOFDR);
    opt_parse.add_opt("params-in", 'P', "HMM parameters file (no training)", 
                      false, params_in_file);
    opt_parse.add_opt("params-out", 'p', "write HMM parameters to this file", 
                      false, params_out_file);
    
    vector<string> leftover_args;
    opt_parse.parse(argc, argv, leftover_args);
    if (argc == 1 || opt_parse.help_requested()) {
      cerr << opt_parse.help_message() << endl
	   << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.about_requested()) {
      cerr << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.option_missing()) {
      cerr << opt_parse.option_missing_message() << endl;
      return EXIT_SUCCESS;
    }
    if (leftover_args.empty()) {
      cerr << opt_parse.help_message() << endl;
      return EXIT_SUCCESS;
    }
    const string cpgs_file = leftover_args.front();
    /****************** END COMMAND LINE OPTIONS *****************/
    
    // separate the regions by chrom and by desert
    vector<SimpleGenomicRegion> cpgs;
    vector< pair<double, double> > meth;
    vector<size_t> time;
    vector<size_t> reads;
    if (VERBOSE)
      cerr << "[READING CPGS AND METH PROPS]" << endl;
    load_cpgs(cpgs_file, cpgs, meth, reads, time);
    if (VERBOSE)
      cerr << "TOTAL CPGS: " << cpgs.size() << endl
      << "MEAN COVERAGE: "
      << accumulate(reads.begin(), reads.end(), 0.0)/reads.size()
      << endl << endl;

    
    rm_missingdata(VERBOSE, cpgs, meth, reads, time);
    
    // set-up distributions
    double fg_alpha = 0;
    double fg_beta = 0;
    double bg_alpha = 0;
    double bg_beta = 0;

    const double n_reads =
      accumulate(reads.begin(), reads.end(), 0.0)/reads.size();
    fg_alpha = 0.33*n_reads;
    fg_beta = 0.67*n_reads;
    bg_alpha = 0.67*n_reads;
    bg_beta = 0.33*n_reads;
    
    BetaBin fg_emission = BetaBin(fg_alpha, fg_beta);
    BetaBin bg_emission = BetaBin(bg_alpha, bg_beta);
   
    double p_sf = 0.5;
    double p_sb = 0.5;
      
    double p_ft = 1e-10;
    double p_bt = 1e-10;

    // transition distribution on time
    double fg_rate = 0.02;
    double bg_rate = 0.002;
   
    // HMM initialization
    TwoVarHMM hmm(tolerance, min_prob, max_iterations, VERBOSE);
    hmm.set_parameters(fg_emission, bg_emission, fg_rate, bg_rate,
                       p_sf, p_sb, p_ft, p_bt);
    
    // HMM training
    double score = hmm.BaumWelchTraining(meth, time);
    
/*
    
    if (!params_out_file.empty()) {
      // WRITE ALL THE HMM PARAMETERS:
      write_params_file(params_out_file, hmm);
    }
 */
 
    /***********************************
     * STEP 5: DECODE THE DOMAINS
     */
    
    
    vector<int> classes;
    vector<double> scores;
    hmm.PosteriorDecoding(meth, time, classes, scores);
    
    
    vector<double> domain_scores;
    get_domain_scores(classes, meth, domain_scores);
    
    vector<double> random_scores;
    shuffle_cpgs(hmm, meth, time, random_scores);
    
    vector<double> p_values;
    assign_p_values(random_scores, domain_scores, p_values);
    
    
    double fdr_cutoff = std::numeric_limits<double>::max();
    if (fdr_cutoff == numeric_limits<double>::max())
      fdr_cutoff = get_fdr_cutoff(p_values, 0.01);

  
    vector<GenomicRegion> domains;
    build_domains(VERBOSE, cpgs, scores, classes, domains);
    
    std::ofstream of;
    if (!outfile.empty()) of.open(outfile.c_str());
    std::ostream out(outfile.empty() ? std::cout.rdbuf() : of.rdbuf());
    
    size_t good_hmr_count = 0;
    for (size_t i = 0; i < domains.size(); ++i)
      if (p_values[i] < fdr_cutoff) {
        domains[i].set_name("HYPO" + smithlab::toa(good_hmr_count++));
        out << domains[i] << '\t' << p_values[i] << '\n';
      } else if (NOFDR) {
        domains[i].set_name("HYPO" + smithlab::toa(good_hmr_count++));
        out << domains[i] << '\t' << p_values[i] << '\n';
      }
    
    // output posterior probabilities
    if (!scores_file.empty()) {
      std::ostream *out_scores = new std::ofstream(scores_file.c_str());
      for (size_t i = 0; i < cpgs.size(); ++i) {
        *out_scores << cpgs[i] << '\t' << scores[i] << endl;
      }
    }

  }
  catch (SMITHLABException &e) {
    cerr << "ERROR:\t" << e.what() << endl;
    return EXIT_FAILURE;
  }
  catch (std::bad_alloc &ba) {
    cerr << "ERROR: could not allocate memory" << endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
