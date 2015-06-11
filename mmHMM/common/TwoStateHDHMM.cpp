/*
  Copyright (C) 2011 University of Southern California
  Authors: Andrew D. Smith, Song Qiang

  This file is part of rmap.

  rmap is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  rmap is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with rmap; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "TwoStateHDHMM.hpp"

#include <iomanip>
#include <numeric>
#include <limits>
#include <cmath>

#include <gsl/gsl_sf_psi.h>
#include <gsl/gsl_sf_gamma.h>

#include "numerical-utils.hpp"

// #define DEBUG

using std::vector;
using std::pair;
using std::setw;
using std::max;
using std::min;
using std::cerr;
using std::endl;
using std::string;
using std::setprecision;

TwoStateHDHMM::TwoStateHDHMM(
    const std::vector<std::pair<double, double> > &_observations,
    const std::vector<size_t> &_reset_points,
    const size_t _MAX_LEN, const double mp, const double tol,
    const size_t max_itr, const bool v) : 
    observations(_observations), reset_points(_reset_points),
    meth_lp(_observations.size()),
    unmeth_lp(_observations.size()),
    fg_log_likelihood(_observations.size()),
    bg_log_likelihood(_observations.size()),
    forward(_observations.size()),
    backward(_observations.size()),
    fg_posteriors(_observations.size()),
    bg_posteriors(_observations.size()),
    MAX_LEN(_MAX_LEN), MIN_PROB(mp), tolerance(tol),
    max_iterations(max_itr), VERBOSE(v) 
{
    for (size_t i = 0; i < observations.size(); ++i)
    {
        const double m = observations[i].first;
        const double u = observations[i].second;
        
        meth_lp[i] = 
            log(std::min(std::max(m/(m + u), 1e-2), 1.0 - 1e-2));
        unmeth_lp[i] = 
            log(std::min(std::max(u/(m + u), 1e-2), 1.0 - 1e-2));
    }
}

void
TwoStateHDHMM::set_parameters(
    const betabin &_fg_emission,
    const betabin &_bg_emission,
    const Distro &_fg_duration,
    const Distro &_bg_duration)
{
    fg_emission = _fg_emission;
    bg_emission = _bg_emission;
    fg_duration = _fg_duration;
    bg_duration = _bg_duration;
    update_observation_likelihood();

    lp_sf = log(0.5);
    lp_sb = log(0.5);
    lp_ft = log(1e-10);
    lp_bt = log(1e-10);
}

void
TwoStateHDHMM::get_parameters(
    betabin &_fg_emission,
    betabin &_bg_emission,
    Distro &_fg_duration,
    Distro &_bg_duration)
{
    _fg_emission = fg_emission;
    _bg_emission = bg_emission;
    _fg_duration = fg_duration;
    _bg_duration = bg_duration;
}


//////////////////////////////////////////////
////// forward and backward algorithms  //////
//////////////////////////////////////////////
void
TwoStateHDHMM::update_observation_likelihood()
{
    fg_log_likelihood.front() = fg_emission(observations.front());
    bg_log_likelihood.front() = bg_emission(observations.front());
    
    for (size_t i = 1; i < observations.size(); ++i)
    {
        fg_log_likelihood[i] =
            fg_log_likelihood[i - 1] + fg_emission(observations[i]);
        bg_log_likelihood[i] =
            bg_log_likelihood[i - 1] + bg_emission(observations[i]);
    }
}

double
TwoStateHDHMM::fg_segment_log_likelihood(
    const size_t start, const size_t end)
{
    return
        (start == 0)
        ? fg_log_likelihood[end - 1]
        : fg_log_likelihood[end - 1] - fg_log_likelihood[start - 1];
}

double
TwoStateHDHMM::bg_segment_log_likelihood(
    const size_t start, const size_t end)
{
    return
        (start == 0)
        ? bg_log_likelihood[end - 1]
        : bg_log_likelihood[end - 1] - bg_log_likelihood[start - 1];
}


double
TwoStateHDHMM::forward_algorithm(const size_t start, const size_t end) 
{
#ifdef DEBUG
    cerr << "check enter forward_algorithm: "<< "OK" << endl;
#endif

    const double self_lp = log(1 - bg_duration.get_params().front());
    const double switch_lp = log(bg_duration.get_params().front());

    std::fill(forward.begin() + start, forward.begin() + end,
              std::make_pair(0.0, 0.0)); 

    forward[start].first =
        lp_sf + fg_segment_log_likelihood(start, start + 1)
        + fg_duration.log_likelihood(1);
    forward[start].second =
        lp_sb + bg_segment_log_likelihood(start, start + 1);

    for (size_t i = start + 1; i < end; ++i)
    {
        // in foreground segment
        const size_t max_len = min(i - start + 1, MAX_LEN) + 1;
        for (size_t l = 1; l < max_len; ++l)
        {
            const size_t beginning = i - l + 1; // inclusive
            const size_t ending = i + 1;       // exclusive

            assert(start <= beginning && beginning < end);
            assert(start < ending && ending <= end);
            assert(ending - beginning == l);

            // foreground segment
            const double fg_seg_llh = (beginning == start) ?      // segment [start, end)
                lp_sf
                + fg_segment_log_likelihood(beginning, ending)
                + fg_duration.log_likelihood(l)
                :
                forward[beginning - 1].second + switch_lp
                + fg_segment_log_likelihood(beginning, ending)
                + fg_duration.log_likelihood(l);

            forward[i].first = log_sum_log(forward[i].first, fg_seg_llh);
        }

        //  in background segment
        forward[i].second =
            log_sum_log(forward[i - 1].first,
                        forward[i - 1].second + self_lp)
            + bg_segment_log_likelihood(i, i + 1);
    }

#ifdef DEBUG
    cerr << "check forward_algorithm: "<< "OK" << endl;
#endif

    return log_sum_log(forward[end - 1].first + lp_ft,
                       forward[end - 1].second + lp_bt);
}

double
TwoStateHDHMM::backward_algorithm(const size_t start, const size_t end)
{
#ifdef DEBUG
    cerr << "check backward_algorithm: "<< "OK" << endl;
#endif
    const int start_int(start), end_int(end);

    const double self_lp = log(1 - bg_duration.get_params().front());
    const double switch_lp = log(bg_duration.get_params().front());

    std::fill(backward.begin() + start, backward.begin() + end,
              std::make_pair(0.0, 0.0)); 

    backward[end - 1].first = lp_ft;
    backward[end - 1].second = lp_bt;

    for (int i = end_int - 2; i >= start_int; --i)
    {
        // observesvation i is foreground segment end
        backward[i].first =
            bg_segment_log_likelihood(i + 1, i + 2)
            + backward[i + 1].second;
        
        // observation i in background segment
        // remain in background
        backward[i].second =
            self_lp
            + bg_segment_log_likelihood(i + 1, i + 2)
            + backward[i + 1].second; 
        
        // switch to a foreground segment
        const size_t max_len = min(end - i - 1, MAX_LEN) + 1;
        for (size_t l = 1; l < max_len; ++l)
        {
            const size_t beginning = i + 1;
            const size_t ending = i + l + 1;

            assert(start <= beginning && beginning < end);
            assert(start < ending && ending <= end);
            assert(ending - beginning == l);

            // foreground segment
            const double fg_seg_llh =       // segment [start, end)
                switch_lp  
                + fg_segment_log_likelihood(beginning, ending)
                + fg_duration.log_likelihood(l)
                + backward[ending - 1].first;
            backward[i].second = log_sum_log(backward[i].second, fg_seg_llh);
        }
   }

    // whole likelihood
    // the first segment is background
    double llh =
        lp_sb
        + bg_segment_log_likelihood(start_int, start_int + 1)
        + backward[start_int].second;
    
    // the first segment is foreground
    const size_t max_len = min(end - start, MAX_LEN) + 1;
    for (size_t l = 1; l < max_len; ++l)
    {
        const size_t beginning = start;
        const size_t ending = start + l;

        assert(start <= beginning && beginning < end);
        assert(start < ending && ending <= end);
        assert(ending - beginning == l);
        
        // foreground segment
        const double fg_seg_llh =       // segment [start, end)
            lp_sf  
            + fg_segment_log_likelihood(beginning, ending)
            + fg_duration.log_likelihood(l)
            + backward[ending - 1].first;

        llh = log_sum_log(llh, fg_seg_llh);
    }

#ifdef DEBUG
    cerr << "check backward_algorithm: "<< "OK" << endl;
#endif

    return llh;
}

//////////////////////////////////////////////
//////       Baum-Welch Training        //////
//////////////////////////////////////////////
// Expectation
void
TwoStateHDHMM::estimate_state_posterior(const size_t start, const size_t end)  
{
#ifdef DEBUG
    cerr << "check enter estimate_state_posterior: "<< "OK" << endl;
#endif

    // const double self_lp = log(1 - bg_duration.get_params().front());
    const double switch_lp = log(bg_duration.get_params().front());
    
    vector<double> fg_evidence(end - start, 0), bg_evidence(end - start, 0);
    for (size_t s = start; s < end; ++s)
    {
        // foreground
        double accu_evidence = 0;
        for (size_t e =  min(s + MAX_LEN, end); e > s; --e)
        {
            const double evidence = (s == start) ?
                lp_sf
                + fg_duration.log_likelihood(e - s)
                + fg_segment_log_likelihood(s, e)
                + backward[e - 1].first
                :
                forward[s - 1].second + switch_lp
                + fg_duration.log_likelihood(e - s)
                + fg_segment_log_likelihood(s, e)
                + backward[e - 1].first;
            
            accu_evidence = log_sum_log(accu_evidence, evidence);
            fg_evidence[e - 1 - start] =
                log_sum_log(fg_evidence[e - 1 - start], accu_evidence);
        }
        
        // background
        bg_evidence[s - start] = forward[s].second + backward[s].second;
    }

    // state posterior
    for (size_t i = start; i < end; ++i)
    {
        const double denom = log_sum_log(fg_evidence[i - start],
                                         bg_evidence[i - start]);
        fg_posteriors[i] = exp(fg_evidence[i - start] - denom);
        bg_posteriors[i] = exp(bg_evidence[i - start] - denom);

        assert(fabs(fg_posteriors[i] + bg_posteriors[i] - 1.0) < 1e-6);
    }

#ifdef DEBUG
    cerr << "check estimate_state_posterior: "<< "OK" << endl;
#endif
}

// Maximization
void 
TwoStateHDHMM::estimate_parameters()
{
// /////
//     cerr << "check enter estimate_parameters: "<< "OK" << endl;
// /////
    static const bool FG_STATE = true;
//    static const bool BG_STATE = false;
    
    fg_emission.fit(meth_lp, unmeth_lp, fg_posteriors);
    bg_emission.fit(meth_lp, unmeth_lp, bg_posteriors);
    update_observation_likelihood();

// /////
//     cerr << "check fg_emission: "<< fg_emission.tostring() << endl;
//     cerr << "check bg_emission: "<< bg_emission.tostring() << endl;
// /////

    
    vector<double> fg_lengths, bg_lengths;
    for (size_t idx = 0; idx < reset_points.size() - 1; ++idx)
    {
        const size_t start = reset_points[idx];
        const size_t end = reset_points[idx + 1];
        bool prev_state = fg_posteriors[start] > bg_posteriors[start];
        size_t len = 1;
        for (size_t i = start + 1; i < end; ++i)
        {
            const bool state = fg_posteriors[i] > bg_posteriors[i];
            if (state == prev_state)
                ++len;
            else
            {
                if (prev_state == FG_STATE)
                    fg_lengths.push_back(len);
                else
                    bg_lengths.push_back(len);

                prev_state = state;
                len = 1;
            }
        }
    }

    if (fg_lengths.size() > 0)
        fg_duration.estimate_params_ml(fg_lengths);

    if (bg_lengths.size() > 0)
        bg_duration.estimate_params_ml(bg_lengths);

// /////
//     cerr << "check estimate_parameters: "<< "OK" << endl;
// /////

}

double
TwoStateHDHMM::single_iteration()
{
// /////
//     cerr << "check enter single_iteration: "<< "OK" << endl;
// /////

    double total_score = 0;

    for (size_t i = 0; i < reset_points.size() - 1; ++i)
    {
        const double forward_score =
            forward_algorithm(reset_points[i], reset_points[i + 1]);
        const double backward_score =
            backward_algorithm(reset_points[i], reset_points[i + 1]);
        
        assert(fabs((forward_score - backward_score)
                    / max(forward_score, backward_score))
                    < 1e-10);
        estimate_state_posterior(reset_points[i], reset_points[i + 1]);
        total_score += forward_score;
    }

    estimate_parameters();

// /////
//     cerr << "check single_iteration: "<< "OK" << endl;
// /////

    return total_score;
}

double
TwoStateHDHMM::BaumWelchTraining()  
{
#ifdef DEBUG
    cerr << "check enter BaumWelchTraining: " << "OK" << endl;
#endif

    if (VERBOSE)
        cerr << setw(5)  << "ITR"
             << setw(16) << "FG Emission"
             << setw(18) << "FG Duration"
             << setw(16) << "BG Emission"
             << setw(16) << "BG Duration"
             << setw(14) << "Likelihood"
             << setw(14) << "DELTA"
             << endl;
  
    double prev_total = -std::numeric_limits<double>::max();
  
    for (size_t i = 0; i < max_iterations; ++i) 
    {
        const betabin old_fg_emission = fg_emission;
        const betabin old_bg_emission = bg_emission;
        const Distro old_fg_duration = fg_duration;
        const Distro old_bg_duration = bg_duration;
        
        double total = single_iteration();
    
        if (VERBOSE)
            cerr << setw(5) << i + 1
                 << setw(16) << old_fg_emission.tostring()
                 << setw(18) << old_fg_duration.tostring()
                 << setw(16) << old_bg_emission.tostring()
                 << setw(16) << old_bg_duration.tostring()
                 << setw(14) << total
                 << setw(14) << (total - prev_total)/std::fabs(total)
                 << endl;

        if ((total - prev_total)/std::fabs(total) < tolerance)
        {
            fg_emission = old_fg_emission;
            bg_emission = old_bg_emission;
            update_observation_likelihood();

            fg_duration = old_fg_duration;
            bg_duration = old_bg_duration;
            
            if (VERBOSE)
                cerr << "CONVERGED" << endl << endl;
            break;
        }
        prev_total = total;
    }

#ifdef DEBUG
     cerr << "check exit BaumWelchTraining: "<< "OK" << endl;
#endif
    return prev_total;
}

//////////////////////////////////////////////
//////          Posterior decoding      //////
//////////////////////////////////////////////
double
TwoStateHDHMM::PosteriorDecoding()
{
// /////
//     cerr << "check enter PosteriorDecoding: "<< "OK" << endl;
// /////

    double total_score = 0;

    for (size_t i = 0; i < reset_points.size() - 1; ++i)
    {
        const double forward_score =
            forward_algorithm(reset_points[i], reset_points[i + 1]);
        const double backward_score =
            backward_algorithm(reset_points[i], reset_points[i + 1]);
        
        assert(fabs((forward_score - backward_score)
                    / max(forward_score, backward_score))
                    < 1e-10);
        estimate_state_posterior(reset_points[i], reset_points[i + 1]);
        total_score += forward_score;
    }

// /////
//     cerr << "check exit PosteriorDecoding: "<< "OK" << endl;
// /////

    return total_score;
}



//////////////////////////////////////////////
//////          export result           //////
//////////////////////////////////////////////

void
TwoStateHDHMM::get_posterior_scores(
    std::vector<double> & scores,
    std::vector<bool> & classes)
{
    scores = fg_posteriors;

    classes.resize(observations.size());
    for (size_t i = 0; i < observations.size(); ++i)
        classes[i] = fg_posteriors[i] > bg_posteriors[i];
}



