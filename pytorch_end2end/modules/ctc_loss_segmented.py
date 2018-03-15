import itertools

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.autograd import Variable

from .warp_ctc_wrapper import WarpCTCLoss
from ..utils.alignment import get_alignment_3d


class CTCLossSegmented(nn.Module):
    def __init__(self, space_idx, blank_idx=0, reduce=False):
        super().__init__()
        self.reduce = reduce
        self.space_idx = space_idx
        self.blank_idx = blank_idx
        self.ctc = WarpCTCLoss(reduce=False)

    def forward(self, logits, targets, logits_lengths, targets_lengths):
        logits_logsoftmax = F.log_softmax(logits, dim=2)
        targets_aligned = get_alignment_3d(logits_logsoftmax, targets, logits_lengths, targets_lengths, is_ctc=True)
        predictions_argmax = torch.max(logits, dim=2)[1].cpu()

        batch_size = logits.size()[0]
        sequence_length = logits.size()[1]

        mask = torch.ByteTensor(batch_size, sequence_length)
        mask.fill_(1)
        for i in range(batch_size):
            start = logits_lengths.data[i]
            if start < sequence_length:
                mask[i, start:] = 0

        targets_well_recognized = (targets_aligned == predictions_argmax) * mask

        cnt_well_recognized = targets_well_recognized.float().sum()
        if cnt_well_recognized < targets_lengths.float().sum() * batch_size / 2:
            # our model is bad, do not try to segment
            return self.ctc(logits, targets, logits_lengths, targets_lengths)

        logits_new = []
        targets_new = []
        logits_lengths_new = []
        targets_lengths_new = []
        batch_ids_new = []

        indices_to_segment = [[0, ] for _ in range(batch_size)]
        for i in range(batch_size):
            start_space = -1
            all_word_well_recognized = True
            for t in range(logits_lengths.data[i]):
                if targets_well_recognized[i, t] == 0:
                    all_word_well_recognized = False
                    continue

                if targets_aligned[i, t] == self.space_idx:
                    if all_word_well_recognized:
                        if start_space != -1 and \
                                (len(indices_to_segment[i]) == 0 or indices_to_segment[i][-1] != start_space):
                            indices_to_segment[i].append(start_space)
                        if t > 0:
                            indices_to_segment[i].append(t)
                    start_space = t
                    all_word_well_recognized = True
            if indices_to_segment[i][-1] != logits_lengths.data[i] - 1:
                indices_to_segment[i].append(logits_lengths.data[i] - 1)
        for i in range(batch_size):
            if len(indices_to_segment[i]) <= 2:  # use full segment
                logits_new.append(logits[i])
                batch_ids_new.append(i)
                targets_new.append(targets[i])
                targets_lengths_new.append(targets_lengths.data[i])
                logits_lengths_new.append(logits_lengths.data[i])
            else:
                for k, start in enumerate(indices_to_segment[i][:-1]):
                    if targets_aligned[i, start] == self.space_idx:
                        batch_ids_new.append(i)
                        targets_new.append(targets_aligned[i, start:start + 1])  # not aligned!
                        logits_new.append(logits[i, start:start + 1])
                        logits_lengths_new.append(1)
                        targets_lengths_new.append(1)
                        start += 1
                    next = indices_to_segment[i][k + 1]
                    if k < len(indices_to_segment[i]) - 2 and targets_aligned[i][next] == self.space_idx:
                        next -= 1
                    if next <= start:
                        continue
                    batch_ids_new.append(i)
                    logits_new.append(logits[i, start:next])
                    current_targets = Variable(torch.LongTensor(
                        [c for c, _ in itertools.groupby(targets_aligned[i, start:next].tolist()) if
                         c != self.blank_idx]))
                    targets_new.append(current_targets)
                    logits_lengths_new.append(next - start)
                    targets_lengths_new.append(current_targets.size()[0])

        new_batch_size = len(batch_ids_new)
        max_logits_len = max(logits_lengths_new)
        max_targets_len = max(targets_lengths_new)

        logits_new_var = Variable(torch.FloatTensor(new_batch_size, max_logits_len, logits.size()[2]), requires_grad=True)
        if logits.is_cuda:
            logits_new_var = logits_new_var.cuda(logits.get_device())
        targets_new_var = Variable(torch.LongTensor(new_batch_size, max_targets_len), requires_grad=False)
        if logits.is_cuda:
            logits_new_var = logits_new_var.cuda(logits_logsoftmax.get_device())

        for i in batch_ids_new:
            logits_new_var[i, :logits_lengths_new[i]] = logits_new[i]
            targets_new_var[i, :targets_lengths_new[i]] = targets_new[i]
        logits_lengths_new = Variable(torch.LongTensor(logits_lengths_new), requires_grad=False)
        targets_lengths_new = Variable(torch.LongTensor(targets_lengths_new), requires_grad=False)

        segmented_loss = self.ctc(logits_new_var, targets_new_var, logits_lengths_new, targets_lengths_new)
        loss = torch.zeros(batch_size)
        if logits.is_cuda:
            loss = loss.cuda(logits.get_device())
        loss = Variable(loss, requires_grad=True)
        for i in batch_ids_new:
            loss[i] += segmented_loss[i]
        return loss