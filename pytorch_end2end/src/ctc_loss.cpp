#include <torch/extension.h>
#include "ctc_loss.h"
#include "math_utils.h"
#include "threadpool.h"

CTCLossWrapper::CTCLossWrapper(int blank_idx_) : blank_idx{blank_idx_} {}

void CTCLossWrapper::_ctc_loss_forward_2d(
        const torch::Tensor& logits,
        const torch::Tensor& targets,
        int sequence_length, int targets_len,
        int batch_i,
        torch::Tensor& losses,
        torch::Tensor& grads) {
    auto logits_2d = logits[batch_i];
    auto targets_1d = targets[batch_i];
    auto num_labels = logits_2d.size(1);
    auto extended_targets_len = targets_len * 2 + 1;
    auto extended_targets = torch::full(extended_targets_len, blank_idx, targets_1d.options());
    for (int i = 0; i < targets_len; i++)
        extended_targets[i * 2 + 1] = targets_1d[i];

    // forward - alpha
    auto log_alpha = torch::full({extended_targets_len, sequence_length}, -INFINITY, logits_2d.options());
    if (sequence_length > 1 || extended_targets_len == 1)
        log_alpha[0][0] = logits_2d[0][extended_targets[0]];
    if (extended_targets_len > 1)
        log_alpha[1][0] = logits_2d[0][extended_targets[1]];
    for (int t = 1; t < sequence_length; t++) { // time step
        auto start = std::max(0, extended_targets_len - 2 * (sequence_length - t));
        auto end = std::min(t * 2 + 2, extended_targets_len);
        for (int j = start; j < end; j++) {
            log_alpha[j][t] = log_alpha[j][t - 1];
            auto current_label = extended_targets[j].item<int>();
            if (j > 0) {
                log_alpha[j][t] = log_sum_exp(log_alpha[j][t], log_alpha[j - 1][t - 1]);
                if (current_label != blank_idx && j - 2 >= 0 &&
                    extended_targets[j - 2].item<int>() != current_label) {
                    log_alpha[j][t] = log_sum_exp(log_alpha[j][t],
                                                  log_alpha[j - 2][t - 1]);
                }
            }
            log_alpha[j][t] += logits_2d[t][current_label];
        }
    }

    if (extended_targets_len > 1)
        losses[batch_i] = -log_sum_exp(log_alpha[extended_targets_len - 1][sequence_length - 1],
                                       log_alpha[extended_targets_len - 2][sequence_length - 1]);
    else
        losses[batch_i] = -log_alpha[extended_targets_len - 1][sequence_length - 1];

    auto loss_forward = -losses[batch_i];
    // backward - beta
    auto log_beta = torch::full_like(log_alpha, -INFINITY);

    if (sequence_length > 1 or extended_targets_len == 1)
        log_beta[extended_targets_len - 1][sequence_length - 1] = 0;
    if (extended_targets_len > 1)
        log_beta[extended_targets_len - 2][sequence_length - 1] = 0;
    for (int t = sequence_length - 2; t >= 0; t--) { // time steps
        auto start = std::max(0, extended_targets_len - 2 * (sequence_length - t));
        auto end = std::min(t * 2 + 2, extended_targets_len);
        for (int j = start; j < end; j++) {
            auto current_label = extended_targets[j].item<int>();
            log_beta[j][t] = log_beta[j][t + 1] + logits_2d[t + 1][extended_targets[j]];
            if (j < extended_targets_len - 1) {
                log_beta[j][t] = log_sum_exp(log_beta[j][t],
                                             log_beta[j + 1][t + 1] + logits_2d[t + 1][extended_targets[j + 1]]);
                if (current_label != blank_idx && j + 2 < extended_targets_len &&
                    extended_targets[j + 2].item<int>() != current_label) {
                    log_beta[j][t] = log_sum_exp(log_beta[j][t], log_beta[j + 2][t + 1] + logits_2d[
                            t + 1][extended_targets[j + 2]]);
                }
            }
        }
    }

    // compute gradient
    auto alpha_beta = log_alpha + log_beta;
    auto prob_sum = torch::full_like(logits_2d, -INFINITY); // sequence_len, alphabet_size
    for (int i = 0; i < extended_targets_len; i++) {
        auto current_label = extended_targets[i].item<int>();
        for (int j = 0; j < sequence_length; j++)
            prob_sum[j][current_label] = log_sum_exp(prob_sum[j][current_label], alpha_beta[i][j]);
    }
    auto negative_term = prob_sum - loss_forward;
    grads[batch_i] = torch::exp(logits_2d) - torch::exp(negative_term);
}

std::tuple<
        at::Tensor,
        at::Tensor
> CTCLossWrapper::ctc_loss_forward(
        const at::Tensor& logits_,
        const at::Tensor& targets_,
        const at::Tensor& logits_lengths_,
        const at::Tensor& targets_lengths_) {

    auto src_device = logits_.device();
    auto work_device = torch::kCPU;

    auto logits = logits_.to(work_device).detach();
    auto targets = targets_.to(work_device);
    auto logits_lengths = logits_lengths_.to(work_device);
    auto targets_lengths = targets_lengths_.to(work_device);

    auto batch_size = logits_lengths.size(0);

    auto options = torch::TensorOptions().dtype(logits.dtype()).layout(torch::kStrided).device(
            work_device).requires_grad(false);
    auto losses = torch::zeros(batch_size, options);
    auto grads = torch::zeros({batch_size, logits.size(1), logits.size(2)}, options);

    {
        ThreadPool pool{static_cast<size_t>(batch_size)};
        for (int i = 0; i < batch_size; i++) {
            pool.add_task([this, &logits, &targets, i, &logits_lengths, &targets_lengths, &losses, &grads] {
                _ctc_loss_forward_2d(logits, targets,
                                     logits_lengths[i].item<int>(), targets_lengths[i].item<int>(),
                                     i, losses, grads);
            });
        }
    }

    losses = losses.to(src_device);
    grads = grads.to(src_device);

    return {losses, grads};
}

