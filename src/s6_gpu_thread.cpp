/*
 * s6_gpu_thread.c
 *
 * Performs spectroscopy of incoming data using s6GPU
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>

#include <cuda.h>
#include <cufft.h>

#include <sched.h>

#include <s6GPU.h>
#include "hashpipe.h"
#include "s6_databuf.h"

#define ELAPSED_NS(start,stop) \
  (((int64_t)stop.tv_sec-start.tv_sec)*1000*1000*1000+(stop.tv_nsec-start.tv_nsec))

int init_gpu_memory(uint64_t num_coarse_chan, device_vectors_t **dv_p, cufftHandle *fft_plan_p, int initial) {
    
    int num_channels_max, num_channels_utilized;

    const char * re[2] = {"re", ""};

    if(num_coarse_chan == 0) {  
        hashpipe_error(__FUNCTION__, "Cannot initialize GPU memory with 0 coarse channels");
        return -1;
    }

    fprintf(stderr, "%sinitializing GPU structures for %ld coarse channels...", initial ? re[1] : re[0], num_coarse_chan);

    if(!initial) {
        delete_device_vectors(*dv_p);
        cufftDestroy(*fft_plan_p);
    }

    // Configure GPU vectors...
    // The maximum number of coarse channels is one determining factor 
    // of input data buffer size and is set at compile time. At run time 
    // the number of coarse channels can change but this does not affect 
    // the size of the input data buffer.  
    num_channels_max = N_COARSE_CHAN   * N_FINE_CHAN;
    num_channels_utilized = num_coarse_chan * N_FINE_CHAN;
    *dv_p = init_device_vectors(num_channels_max, num_channels_utilized, N_POLS_PER_BEAM);

    // Configure cuFFT...
    size_t  nfft_     = N_FINE_CHAN;                      // FFT length
    size_t  nbatch    = num_coarse_chan*N_POLS_PER_BEAM;  // number of FFT batches to do 
                                                          //    (only work on utilized coarse channels)
    int     istride   = N_COARSE_CHAN*N_POLS_PER_BEAM;    // this effectively transposes the input data
    int     ostride   = 1;                                // no transpose needed on the output
    int     idist     = 1;                                // distance between 1st input elements of consecutive batches
    int     odist     = nfft_;                            // distance between 1st output elements of consecutive batches
    create_fft_plan_1d_c2c(fft_plan_p, istride, idist, ostride, odist, nfft_, nbatch);

    fprintf(stderr, "done\n");

    return 0;
}

static void *run(hashpipe_thread_args_t * args)
{
    // Local aliases to shorten access to args fields
    s6_input_databuf_t *db_in = (s6_input_databuf_t *)args->ibuf;
    s6_output_databuf_t *db_out = (s6_output_databuf_t *)args->obuf;
    hashpipe_status_t st = args->st;
    const char * status_key = args->thread_desc->skey;

#ifdef DEBUG_SEMS
    fprintf(stderr, "s/tid %lu/                      GPU/\n", pthread_self());
#endif

    int rv;
    uint64_t start_mcount, last_mcount=0;
    int s6gpu_error = 0;
    int curblock_in=0;
    int curblock_out=0;
    int error_count = 0, max_error_count = 0;
    float error, max_error = 0.0;

    struct timespec start, stop;
    uint64_t elapsed_gpu_ns  = 0;
    uint64_t gpu_block_count = 0;

#if 0
    // raise this thread to maximum scheduling priority
    struct sched_param SchedParam;
    int retval;
    SchedParam.sched_priority = sched_get_priority_max(SCHED_FIFO);
    fprintf(stderr, "Setting scheduling priority to %d\n", SchedParam.sched_priority);
    retval = sched_setscheduler(0, SCHED_FIFO, &SchedParam);
    if(retval) {
        perror("sched_setscheduler :");
    }
#endif

    // init s6GPU
    int gpu_dev=0;          // default to 0
    int maxhits = MAXHITS; // default
    hashpipe_status_lock_safe(&st);
    hgeti4(st.buf, "GPUDEV", &gpu_dev);
    hgeti4(st.buf, "MAXHITS", &maxhits);
    hashpipe_status_unlock_safe(&st);
    init_device(gpu_dev);
    
    // pin the databufs from cudu's point of view
    cudaHostRegister((void *) db_in, sizeof(s6_input_databuf_t), cudaHostRegisterPortable);
    cudaHostRegister((void *) db_out, sizeof(s6_output_databuf_t), cudaHostRegisterPortable);

    cufftHandle fft_plan;
    cufftHandle *fft_plan_p = &fft_plan;

    device_vectors_t *dv_p = NULL;
    uint64_t num_coarse_chan = N_COARSE_CHAN;
    init_gpu_memory(num_coarse_chan, &dv_p, fft_plan_p, 1);

    while (run_threads()) {

        hashpipe_status_lock_safe(&st);
        hputi4(st.buf, "GPUBLKIN", curblock_in);
        hputs(st.buf, status_key, "waiting");
        hputi4(st.buf, "GPUBKOUT", curblock_out);
        hashpipe_status_unlock_safe(&st);

        // Wait for new input block to be filled
        while ((rv=hashpipe_databuf_wait_filled((hashpipe_databuf_t *)db_in, curblock_in)) != HASHPIPE_OK) {
            if (rv==HASHPIPE_TIMEOUT) {
                hashpipe_status_lock_safe(&st);
                hputs(st.buf, status_key, "blocked");
                hashpipe_status_unlock_safe(&st);
                continue;
            } else {
                hashpipe_error(__FUNCTION__, "error waiting for filled databuf");
                pthread_exit(NULL);
                break;
            }
        }

        // Got a new data block, update status and determine how to handle it
        hashpipe_status_lock_safe(&st);
        hputu8(st.buf, "GPUMCNT", db_in->block[curblock_in].header.mcnt);
        hashpipe_status_unlock_safe(&st);

        if(db_in->block[curblock_in].header.num_coarse_chan != num_coarse_chan) {
            // number of coarse channels has changed!  Redo GPU memory / FFT plan
            num_coarse_chan = db_in->block[curblock_in].header.num_coarse_chan;
            init_gpu_memory(num_coarse_chan, &dv_p, fft_plan_p, 0);
        }

        if(db_in->block[curblock_in].header.mcnt >= last_mcount) {
          // Wait for new output block to be free
          while ((rv=s6_output_databuf_wait_free(db_out, curblock_out)) != HASHPIPE_OK) {
              if (rv==HASHPIPE_TIMEOUT) {
                  hashpipe_status_lock_safe(&st);
                  hputs(st.buf, status_key, "blocked gpu out");
                  hashpipe_status_unlock_safe(&st);
                  continue;
              } else {
                  hashpipe_error(__FUNCTION__, "error waiting for free databuf");
                  pthread_exit(NULL);
                  break;
              }
          }
        }

        // Note processing status
        hashpipe_status_lock_safe(&st);
        hputs(st.buf, status_key, "processing gpu");
        hashpipe_status_unlock_safe(&st);

        clock_gettime(CLOCK_MONOTONIC, &start);

        // pass input metadata to output
        db_out->block[curblock_out].header.mcnt            = db_in->block[curblock_in].header.mcnt;
        db_out->block[curblock_out].header.coarse_chan_id  = db_in->block[curblock_in].header.coarse_chan_id;
        db_out->block[curblock_out].header.num_coarse_chan = db_in->block[curblock_in].header.num_coarse_chan;
        memcpy(&db_out->block[curblock_out].header.missed_pkts, 
               &db_in->block[curblock_in].header.missed_pkts, 
               sizeof(uint64_t) * N_BEAM_SLOTS);

        // only do spectroscopy if there are more than zero channels!
        if(num_coarse_chan) {
            // do spectroscopy and hit detection on this block.
            // spectroscopy() writes directly to the output buffer.
            size_t total_hits = 0;
            for(int beam_i = 0; beam_i < N_BEAMS; beam_i++) {
                size_t nhits = 0; 
                // TODO there is no real c error checking in spectroscopy()
                //      Errors are handled via c++ exceptions
                nhits = spectroscopy(num_coarse_chan,
                                     N_FINE_CHAN,
                                     N_POLS_PER_BEAM,
                                     beam_i,
                                     maxhits,
                                     MAXGPUHITS,
                                     POWER_THRESH,
                                     SMOOTH_SCALE,
                                     &db_in->block[curblock_in].data[beam_i*N_BYTES_PER_BEAM/sizeof(uint64_t)],
                                     N_BYTES_PER_BEAM,
                                     &db_out->block[curblock_out],
                                     dv_p,
                                     fft_plan_p);
//fprintf(stderr, "spectroscopy() returned %ld for beam %d\n", nhits, beam_i);
                total_hits += nhits;
                clock_gettime(CLOCK_MONOTONIC, &stop);
                elapsed_gpu_ns += ELAPSED_NS(start, stop);
                gpu_block_count++;
            }  //  for(int beam_i = 0; beam_i < N_BEAMS; beam_i++)
        }  // if(num_coarse_chan)

        hashpipe_status_lock_safe(&st);
        hputr4(st.buf, "GPUMXERR", max_error);
        hputi4(st.buf, "GPUERCNT", error_count);
        hputi4(st.buf, "GPUMXECT", max_error_count);
        hashpipe_status_unlock_safe(&st);

        // Mark output block as full and advance
        s6_output_databuf_set_filled(db_out, curblock_out);
        curblock_out = (curblock_out + 1) % db_out->header.n_block;

        // Mark input block as free and advance
        //memset((void *)&db_in->block[curblock_in], 0, sizeof(s6_input_block_t));     // TODO re-init first
        hashpipe_databuf_set_free((hashpipe_databuf_t *)db_in, curblock_in);
        curblock_in = (curblock_in + 1) % db_in->header.n_block;

        /* Check for cancel */
        pthread_testcancel();
    }

    // Thread success!
    // unpin the databufs from cudu's point of view
    cudaHostUnregister((void *) db_in);
    cudaHostUnregister((void *) db_out);
    return NULL;
}

static hashpipe_thread_desc_t gpu_thread = {
    name: "s6_gpu_thread",
    skey: "GPUSTAT",
    init: NULL,
    run:  run,
    ibuf_desc: {s6_input_databuf_create},
    obuf_desc: {s6_output_databuf_create}
};

static __attribute__((constructor)) void ctor()
{
  register_hashpipe_thread(&gpu_thread);
}
