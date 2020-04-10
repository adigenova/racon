/*!
 * @file cudaaligner.cpp
 *
 * @brief CUDABatchAligner class source file
 */

#include <claragenomics/utils/cudautils.hpp>

#include "cudaaligner.hpp"

namespace racon {

std::atomic<uint32_t> CUDABatchAligner::batches;

std::unique_ptr<CUDABatchAligner> createCUDABatchAligner(uint32_t max_query_size,
                                                         uint32_t max_target_size,
                                                         uint32_t max_alignments,
                                                         uint32_t device_id)
{
    return std::unique_ptr<CUDABatchAligner>(new CUDABatchAligner(max_query_size,
                                                                  max_target_size,
                                                                  max_alignments,
                                                                  device_id));
}

CUDABatchAligner::CUDABatchAligner(uint32_t max_query_size,
                                   uint32_t max_target_size,
                                   uint32_t max_alignments,
                                   uint32_t device_id)
    : overlaps_()
    , stream_(0)
{
    bid_ = CUDABatchAligner::batches++;

    CGA_CU_CHECK_ERR(cudaSetDevice(device_id));

    CGA_CU_CHECK_ERR(cudaStreamCreate(&stream_));

    aligner_ = claragenomics::cudaaligner::create_aligner(max_query_size,
                                                          max_target_size,
                                                          max_alignments,
                                                          claragenomics::cudaaligner::AlignmentType::global,
                                                          stream_,
                                                          device_id);
}

CUDABatchAligner::~CUDABatchAligner()
{
    CGA_CU_CHECK_ERR(cudaStreamDestroy(stream_));
}

bool CUDABatchAligner::addOverlap(Overlap* overlap,
    const std::vector<std::unique_ptr<biosoup::Sequence>>& targets,
    const std::vector<std::unique_ptr<biosoup::Sequence>>& sequences)
{
    const char* q = &(sequences[overlap->q_id]->data[overlap->q_begin]);
    int32_t q_len = overlap->q_end - overlap->q_begin;
    const char* t = &(targets[overlap->t_id]->data[overlap->t_begin]);
    int32_t t_len = overlap->t_end - overlap->t_begin;

    // NOTE: The cudaaligner API for adding alignments is the opposite of edlib. Hence, what is
    // treated as target in edlib is query in cudaaligner and vice versa.
    claragenomics::cudaaligner::StatusType s = aligner_->add_alignment(t, t_len,
                                                                       q, q_len);
    if (s == claragenomics::cudaaligner::StatusType::exceeded_max_alignments)
    {
        return false;
    }
    else if (s == claragenomics::cudaaligner::StatusType::exceeded_max_alignment_difference
             || s == claragenomics::cudaaligner::StatusType::exceeded_max_length)
    {
        cpu_overlap_data_.emplace_back(std::make_pair<std::string, std::string>(std::string(q, q + q_len),
                                                                                std::string(t, t + t_len)));
        cpu_overlaps_.push_back(overlap);
    }
    else if (s != claragenomics::cudaaligner::StatusType::success)
    {
        fprintf(stderr, "Unknown error in cuda aligner!\n");
    }
    else
    {
        overlaps_.push_back(overlap);
    }
    return true;
}

void CUDABatchAligner::alignAll()
{
    aligner_->align_all();
    compute_cpu_overlaps();
}

void CUDABatchAligner::compute_cpu_overlaps()
{
    for(std::size_t a = 0; a < cpu_overlaps_.size(); a++)
    {
        // Run CPU version of overlap.
        Overlap* overlap = cpu_overlaps_[a];
        overlap->Align(cpu_overlap_data_[a].first.c_str(),
            cpu_overlap_data_[a].first.length(),
            cpu_overlap_data_[a].second.c_str(),
            cpu_overlap_data_[a].second.length());
    }
}

void CUDABatchAligner::find_breaking_points(uint32_t window_length)
{
    aligner_->sync_alignments();

    const std::vector<std::shared_ptr<claragenomics::cudaaligner::Alignment>>& alignments = aligner_->get_alignments();
    // Number of alignments should be the same as number of overlaps.
    if (overlaps_.size() != alignments.size())
    {
        throw std::runtime_error("Number of alignments doesn't match number of overlaps in cudaaligner.");
    }
    for(std::size_t a = 0; a < alignments.size(); a++)
    {
        overlaps_[a]->cigar = alignments[a]->convert_to_cigar();
        overlaps_[a]->FindBreakPoints(window_length);
    }
    for(Overlap* overlap : cpu_overlaps_)
    {
        // Run CPU version of breaking points.
        overlap->FindBreakPoints(window_length);
    }
}

void CUDABatchAligner::reset()
{
    overlaps_.clear();
    cpu_overlaps_.clear();
    cpu_overlap_data_.clear();
    aligner_->reset();
}

}
