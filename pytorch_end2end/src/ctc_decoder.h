#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <torch/extension.h>
#include "lm/model.hh"

using word2index_t = std::unordered_map<std::string, lm::WordIndex>;
using lm_state_t = lm::ngram::State;

class CTCDecoder {
public:
    CTCDecoder(int blank_idx_, int beam_width_,
               std::vector<std::string> labels_,
               const std::string& lm_path, bool case_sensitive_, double lmwt_);

    lm::WordIndex get_idx(const std::string& word);

    void print_scores_for_sentence(std::vector<std::string> words);

    std::tuple<
            at::Tensor,
            at::Tensor,
            std::vector<std::string>
    > decode_greedy(const at::Tensor& logits,
                    const at::Tensor& logits_lengths);

    std::tuple<
            at::Tensor,
            at::Tensor,
            std::vector<std::string>
    > decode(const at::Tensor& logits,
             const at::Tensor& logits_lengths);

private:
    int blank_idx;
    int space_idx;
    int beam_width;
    bool case_sensitive;
    double lmwt;
    double oov_score;
    std::vector<std::string> labels;
    std::unique_ptr<lm::ngram::ProbingModel> lm_model;
    word2index_t word2index;

    std::tuple<std::vector<int>, int, std::string> decode_sentence(const at::Tensor& logits_2d, int sequence_len);
    double get_score_for_sentence(std::vector<std::string> words);
    std::string indices2str(const std::vector<int>& char_ids);
    std::string indices2str(const at::Tensor& char_ids, int len);
    bool is_empty_sentence(const std::vector<int>& sentence);
    double get_score_for_sentence(const std::vector<int>& sentence);
};


PYBIND11_MODULE(cpp_ctc_decoder, m) {
    namespace py = pybind11;
    using namespace pybind11::literals;
    py::class_<CTCDecoder>(m, "CTCDecoder").
            def(py::init<int, int, std::vector<std::string>, std::string, bool, double>(),
                "blank_idx"_a,
                "beam_width_"_a = 100,
                "labels"_a = std::vector<std::string>{},
                "lm_path"_a = "",
                "case_sensitive"_a = false,
                "lmwt_"_a = 1.0).
            def("decode_greedy", &CTCDecoder::decode_greedy, "Decode greedy", "logits"_a, "logits_lengths"_a).
            def("decode", &CTCDecoder::decode, "Decode greedy", "logits"_a, "logits_lengths"_a).
            def("print_scores_for_sentence", &CTCDecoder::print_scores_for_sentence, "Print scores", "words"_a);
}
