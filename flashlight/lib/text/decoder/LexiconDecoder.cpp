 /*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <unordered_map>
 
#include "flashlight/lib/text/decoder/LexiconDecoder.h"
#include "flashlight/lib/text/decoder/Utils.h"

namespace fl {
namespace lib {
namespace text {

void LexiconDecoder::decodeBegin() {
  hyp_.clear(); // Vector of hypothesis for all the frames so far
  hyp_.emplace(0, std::vector<LexiconDecoderState>());

  /* note: the lm reset itself with :start() */
  hyp_[0].emplace_back(
      0.0, lm_->start(0), lexicon_->getRoot(), nullptr, sil_, -1);
  nDecodedFrames_ = 0;
  nPrunedFrames_ = 0;
}

void LexiconDecoder::decodeStep(const float* emissions, int T, int N) {      
  int startFrame = nDecodedFrames_ - nPrunedFrames_;
  // Extend hyp_ buffer
  if (hyp_.size() < startFrame + T + 2) {
    for (int i = hyp_.size(); i < startFrame + T + 2; i++) {
      hyp_.emplace(i, std::vector<LexiconDecoderState>());
    }
  }

  std::vector<size_t> idx(N);
  for (int t = 0; t < T; t++) {
    std::iota(idx.begin(), idx.end(), 0);
    if (N > opt_.beamSizeToken) {
      std::partial_sort(
          idx.begin(),
          idx.begin() + opt_.beamSizeToken,
          idx.end(),
          [&t, &N, &emissions](const size_t& l, const size_t& r) {
            return emissions[t * N + l] > emissions[t * N + r];
          });
    }

    candidatesReset(candidatesBestScore_, candidates_, candidatePtrs_);
    for (const LexiconDecoderState& prevHyp : hyp_[startFrame + t]) {
      const TrieNode* prevLex = prevHyp.lex;
      const int prevIdx = prevHyp.token;
      const float lexMaxScore =
          prevLex == lexicon_->getRoot() ? 0 : prevLex->maxScore;

      /* (1) Try children */
      for (int r = 0; r < std::min(opt_.beamSizeToken, N); ++r) {
        //for each symbol (token) in the emissions at time t, 
        //compute the emittingModelScore
        int n = idx[r]; //n = token of the current symbol being analised
        auto iter = prevLex->children.find(n);
        if (iter == prevLex->children.end()) {
          continue;
        }
        const TrieNodePtr& lex = iter->second;
        double emittingModelScore = emissions[t * N + n]; //use the emission[t, n] as score
        if (nDecodedFrames_ + t > 0 &&
            opt_.criterionType == CriterionType::ASG) {
          emittingModelScore += transitions_[n * N + prevIdx]; //not used by CTC
        }
        double score = prevHyp.score + emittingModelScore; //combine with previous hypotesis score
        if (n == sil_) {
          score += opt_.silScore;
        } 

        LMStatePtr lmState;
        double lmScore = 0.;

        if (isLmToken_) {
          auto lmStateScorePair = lm_->score(prevHyp.lmState, n);
          lmState = lmStateScorePair.first;
          lmScore = lmStateScorePair.second;
        }

        // We eat-up a new token
        if (opt_.criterionType != CriterionType::CTC || prevHyp.prevBlank ||
            n != prevIdx) {
          if (!lex->children.empty()) {
            if (!isLmToken_) {
              lmState = prevHyp.lmState;
              lmScore = lex->maxScore - lexMaxScore;
            }
            candidatesAdd(
                candidates_,
                candidatesBestScore_,
                opt_.beamThreshold,
                score + opt_.lmWeight * lmScore,
                lmState,
                lex.get(),
                &prevHyp,
                n,
                -1,
                false, // prevBlank
                prevHyp.emittingModelScore + emittingModelScore,
                prevHyp.lmScore + lmScore);
          }
        }

        // If we got a true word
        for (auto label : lex->labels) {
          if (prevLex == lexicon_->getRoot() && prevHyp.token == n) {
            // This is to avoid an situation that, when there is word with
            // single token spelling (e.g. X -> x) in the lexicon and token `x`
            // is predicted in several consecutive frames, multiple word `X`
            // will be emitted. This violates the property of CTC, where
            // there must be an blank token in between to predict 2 identical
            // tokens consecutively.
            continue;
          }
            
          if (!isLmToken_) {
            auto lmStateScorePair = lm_->score(prevHyp.lmState, label);
            lmState = lmStateScorePair.first;
            lmScore = lmStateScorePair.second - lexMaxScore;
          }
                        
          //Improve score of words present in the custom vocabulary
          auto total_score = score + (opt_.lmWeight * lmScore) + opt_.wordScore;
          auto str_label = std::to_string(label);
          if (dict_custom_vocab_.contains(str_label)) {
              auto word_len = lex->depth - 1;
              //score_increment is inversely proportional to word_len
              //Note that 15 was computed based on the largest word in the custom vocabulary, but don't necessarily
              //need to be changed if we add some larger word.
              auto score_increment = std::fabs(total_score * opt_.customWordFactor / (15/word_len));
              //if (word_len <= 3) {
              //    score_increment = (int)score_increment * 0.5;
              //}
              total_score += score_increment;
          }
            
          candidatesAdd(
              candidates_,
              candidatesBestScore_,
              opt_.beamThreshold,              
              total_score, // includes custom_vocab_score in the score math
              lmState,
              lexicon_->getRoot(),
              &prevHyp,
              n,
              label,
              false, // prevBlank
              prevHyp.emittingModelScore + emittingModelScore,
              prevHyp.lmScore + lmScore);
        }

        // If we got an unknown word
        if (lex->labels.empty() && (opt_.unkScore > kNegativeInfinity)) {
          if (!isLmToken_) {
            auto lmStateScorePair = lm_->score(prevHyp.lmState, unk_);
            lmState = lmStateScorePair.first;
            lmScore = lmStateScorePair.second - lexMaxScore;
          }
          candidatesAdd(
              candidates_,
              candidatesBestScore_,
              opt_.beamThreshold,
              score + opt_.lmWeight * lmScore + opt_.unkScore,
              lmState,
              lexicon_->getRoot(),
              &prevHyp,
              n,
              unk_,
              false, // prevBlank
              prevHyp.emittingModelScore + emittingModelScore,
              prevHyp.lmScore + lmScore);
        }
      }

      /* (2) Try same lexicon node */
      if (opt_.criterionType != CriterionType::CTC || !prevHyp.prevBlank ||
          prevLex == lexicon_->getRoot()) {
        int n = prevLex == lexicon_->getRoot() ? sil_ : prevIdx;
        double emittingModelScore = emissions[t * N + n];
        if (nDecodedFrames_ + t > 0 &&
            opt_.criterionType == CriterionType::ASG) {
          emittingModelScore += transitions_[n * N + prevIdx];
        }
        double score = prevHyp.score + emittingModelScore;
        if (n == sil_) {
          score += opt_.silScore;
        }

        candidatesAdd(
            candidates_,
            candidatesBestScore_,
            opt_.beamThreshold,
            score,
            prevHyp.lmState,
            prevLex,
            &prevHyp,
            n,
            -1,
            false, // prevBlank
            prevHyp.emittingModelScore + emittingModelScore,
            prevHyp.lmScore);
      }

      /* (3) CTC only, try blank */
      if (opt_.criterionType == CriterionType::CTC) {
        int n = blank_;
        double emittingModelScore = emissions[t * N + n];
        candidatesAdd(
            candidates_,
            candidatesBestScore_,
            opt_.beamThreshold,
            prevHyp.score + emittingModelScore,
            prevHyp.lmState,
            prevLex,
            &prevHyp,
            n,
            -1,
            true, // prevBlank
            prevHyp.emittingModelScore + emittingModelScore,
            prevHyp.lmScore);
      }
      // finish proposing
    }

    candidatesStore(
        candidates_,
        candidatePtrs_,
        hyp_[startFrame + t + 1],
        opt_.beamSize,
        candidatesBestScore_ - opt_.beamThreshold,
        opt_.logAdd,
        false);
    updateLMCache(lm_, hyp_[startFrame + t + 1]);      
  }
  nDecodedFrames_ += T;
}

void LexiconDecoder::decodeEnd() {
  candidatesReset(candidatesBestScore_, candidates_, candidatePtrs_);
  bool hasNiceEnding = false;
  for (const LexiconDecoderState& prevHyp :
       hyp_[nDecodedFrames_ - nPrunedFrames_]) {
    if (prevHyp.lex == lexicon_->getRoot()) {
      hasNiceEnding = true;
      break;
    }
  }
  for (const LexiconDecoderState& prevHyp :
       hyp_[nDecodedFrames_ - nPrunedFrames_]) {
    const TrieNode* prevLex = prevHyp.lex;
    const LMStatePtr& prevLmState = prevHyp.lmState;

    if (!hasNiceEnding || prevHyp.lex == lexicon_->getRoot()) {
      auto lmStateScorePair = lm_->finish(prevLmState);
      auto lmScore = lmStateScorePair.second;
      candidatesAdd(
          candidates_,
          candidatesBestScore_,
          opt_.beamThreshold,
          prevHyp.score + opt_.lmWeight * lmScore,
          lmStateScorePair.first,
          prevLex,
          &prevHyp,
          sil_,
          -1,
          false, // prevBlank
          prevHyp.emittingModelScore,
          prevHyp.lmScore + lmScore);
    }
  }

  candidatesStore(
      candidates_,
      candidatePtrs_,
      hyp_[nDecodedFrames_ - nPrunedFrames_ + 1],
      opt_.beamSize,
      candidatesBestScore_ - opt_.beamThreshold,
      opt_.logAdd,
      true);
  ++nDecodedFrames_;
}

std::vector<DecodeResult> LexiconDecoder::getAllFinalHypothesis() const {
  int finalFrame = nDecodedFrames_ - nPrunedFrames_;
  if (finalFrame < 1) {
    return std::vector<DecodeResult>{};
  }

  return getAllHypothesis(hyp_.find(finalFrame)->second, finalFrame);
}

DecodeResult LexiconDecoder::getBestHypothesis(int lookBack) const {
  if (nDecodedFrames_ - nPrunedFrames_ - lookBack < 1) {
    return DecodeResult();
  }

  const LexiconDecoderState* bestNode = findBestAncestor(
      hyp_.find(nDecodedFrames_ - nPrunedFrames_)->second, lookBack);
  return getHypothesis(bestNode, nDecodedFrames_ - nPrunedFrames_ - lookBack);
}

int LexiconDecoder::nHypothesis() const {
  int finalFrame = nDecodedFrames_ - nPrunedFrames_;
  return hyp_.find(finalFrame)->second.size();
}

int LexiconDecoder::nDecodedFramesInBuffer() const {
  return nDecodedFrames_ - nPrunedFrames_ + 1;
}

void LexiconDecoder::prune(int lookBack) {
  if (nDecodedFrames_ - nPrunedFrames_ - lookBack < 1) {
    return; // Not enough decoded frames to prune
  }

  /* (1) Find the last emitted word in the best path */
  const LexiconDecoderState* bestNode = findBestAncestor(
      hyp_.find(nDecodedFrames_ - nPrunedFrames_)->second, lookBack);
  if (!bestNode) {
    return; // Not enough decoded frames to prune
  }

  int startFrame = nDecodedFrames_ - nPrunedFrames_ - lookBack;
  if (startFrame < 1) {
    return; // Not enough decoded frames to prune
  }

  /* (2) Move things from back of hyp_ to front and normalize scores */
  pruneAndNormalize(hyp_, startFrame, lookBack);

  nPrunedFrames_ = nDecodedFrames_ - lookBack;    
}

} // namespace text
} // namespace lib
} // namespace fl
