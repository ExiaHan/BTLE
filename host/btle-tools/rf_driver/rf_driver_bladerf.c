#include "rf_driver_cfg.h"

#ifdef HAS_BLADERF
#include <pthread.h>
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <libbladeRF.h>
#include "rf_driver_bladerf.h"
#include "../common_misc.h"

extern pthread_mutex_t callback_lock;
extern void sigint_callback_handler(int signum);

static struct bladerf_stream *bladerf_rx_stream;
static pthread_t bladerf_rx_task;
static struct bladerf_data bladerf_rx_data;

static volatile int16_t bladerf_tx_buf[NUM_BLADERF_BUF_SAMPLE_TX*2];
static volatile int rx_buf_offset=0;
static volatile IQ_TYPE rx_buf[LEN_BUF + LEN_BUF_MAX_NUM_PHY_SAMPLE];

void *bladerf_stream_callback(struct bladerf *dev, struct bladerf_stream *stream,
                      struct bladerf_metadata *metadata, void *samples,
                      size_t num_samples, void *user_data)
{
  struct bladerf_data *my_data = (struct bladerf_data *)user_data;
      size_t i;
      int16_t *sample = (int16_t *)samples ;
      if (num_samples>0) {
        pthread_mutex_lock(&callback_lock);
        for(i = 0; i < num_samples ; i++ ) {
            rx_buf[rx_buf_offset] = (((*sample)>>4)&0xFF);
            rx_buf[rx_buf_offset+1] = (((*(sample+1))>>4)&0xFF);
            rx_buf_offset = (rx_buf_offset+2)&( LEN_BUF-1 ); //cyclic buffer

            sample += 2 ;
        }
        pthread_mutex_unlock(&callback_lock);
      }
    if (do_exit) {
        return NULL;
    } else {
        void *rv = my_data->buffers[my_data->idx];
        my_data->idx = (my_data->idx + 1) % my_data->num_buffers;
        return rv ;
    }
}

int bladerf_get_rx_sample(void *dev, void *buf, int *len) {
  static phase = 0;
  int rx_buf_offset_tmp;
  int sample_ready_flag = 0;
  IQ_TYPE *rxp;

  rx_buf_offset_tmp = rx_buf_offset - LEN_BUF_MAX_NUM_PHY_SAMPLE;
  // cross point 0
  if (rx_buf_offset_tmp>=0 && rx_buf_offset_tmp<(LEN_BUF/2) && phase==1) {
    //printf("rx_buf_offset cross 0: %d %d %d\n", rx_buf_offset, (LEN_BUF/2), LEN_BUF_MAX_NUM_PHY_SAMPLE);
    phase = 0;
    memcpy((void *)(rx_buf+LEN_BUF), (void *)rx_buf, LEN_BUF_MAX_NUM_PHY_SAMPLE*sizeof(IQ_TYPE));
    rxp = (IQ_TYPE*)(rx_buf + (LEN_BUF/2));
    sample_ready_flag = 1;
  }

  // cross point 1
  if (rx_buf_offset_tmp>=(LEN_BUF/2) && phase==0) {
    //printf("rx_buf_offset cross 1: %d %d %d\n", rx_buf_offset, (LEN_BUF/2), LEN_BUF_MAX_NUM_PHY_SAMPLE);
    phase = 1;
    rxp = (IQ_TYPE*)rx_buf;
    sample_ready_flag = 1;
  }

  (*buf) = rxp;

  return(sample_ready_flag);
}

int bladerf_update_rx_gain(void *device, int *gain) {
  int result;
  struct bladerf *dev = device;
  
  if ((*gain)==-1) {
    (*gain) = bladerf_rx_gain_internal;
    return(0);
  }

  if ((*gain)!=bladerf_rx_gain_internal) {
    status = bladerf_set_gain(dev, BLADERF_MODULE_RX,(*gain));
    if( result != 0 ) {
      fprintf(stderr, "bladerf_update_rx_gain: Failed to set rx gain: %s\n", bladerf_strerror(status));
      return(-1);
    } else {
      bladerf_rx_gain_internal = (*gain);
    }
  }

  return(0);
}

int bladerf_update_tx_gain(void *device, int *gain) {
  int result;
  struct bladerf *dev = device;
  
  if ((*gain)==-1) {
    (*gain) = bladerf_tx_gain_internal;
    return(0);
  }

  if ((*gain)!=bladerf_tx_gain_internal) {
    status = bladerf_set_gain(dev, BLADERF_MODULE_TX,(*gain));
    if( result != 0 ) {
      fprintf(stderr, "bladerf_update_tx_gain: Failed to set tx gain: %s\n", bladerf_strerror(status));
      return(-1);
    } else {
      bladerf_tx_gain_internal = (*gain);
    }
  }

  return(0);
}

int bladerf_update_tx_freq(void *dev, uint64_t *freq_hz) {
  int status;
  
  if ((*freq_hz)==0) {
    (*freq_hz) = bladerf_tx_freq_internal;
    return(0);
  }

  if ((*freq_hz)!=bladerf_tx_freq_internal) {
    status = bladerf_set_frequency((struct bladerf *)dev, BLADERF_MODULE_TX,(*freq_hz));
    if (status != 0) {
        fprintf(stderr, "bladerf_update_tx_freq: Failed to set tx frequency: %s\n", bladerf_strerror(status));
        return(-1);
    } else {
      bladerf_tx_freq_internal = (*freq_hz);
    }
  }

  return(0);
}

int bladerf_update_rx_freq(void *dev, uint64_t *freq_hz) {
  int status;
  
  if ((*freq_hz)==0) {
    (*freq_hz) = bladerf_rx_freq_internal;
    return(0);
  }

  if ((*freq_hz)!=bladerf_rx_freq_internal) {
    status = bladerf_set_frequency((struct bladerf *)dev, BLADERF_MODULE_RX,(*freq_hz));
    if (status != 0) {
        fprintf(stderr, "bladerf_update_rx_freq: Failed to set rx frequency: %s\n", bladerf_strerror(status));
        return(-1);
    } else {
      bladerf_rx_freq_internal = (*freq_hz);
    }
  }

  return(0);
}

int bladerf_update_tx_rate(void *dev, int *rate) {
  int status, actual;
  
  if ((*rate)==-1) {
    (*rate) = bladerf_tx_rate_internal;
    return(0);
  }

  if ((*rate)!=bladerf_tx_rate_internal) {
    status = bladerf_set_sample_rate((struct bladerf *)dev, BLADERF_MODULE_TX,(*rate),&actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_update_tx_rate: Failed to set tx sample rate: %s\n", bladerf_strerror(status));
        return(-1);
    } else {
      bladerf_tx_rate_internal = (*rate);
    }
  }

  return(0);
}

int bladerf_update_rx_rate(void *dev, int *rate) {
  int status, actual;
  
  if ((*rate)==-1) {
    (*rate) = bladerf_rx_rate_internal;
    return(0);
  }

  if ((*rate)!=bladerf_rx_rate_internal) {
    status = bladerf_set_sample_rate((struct bladerf *)dev, BLADERF_MODULE_RX,(*rate),&actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_update_rx_rate: Failed to set rx sample rate: %s\n", bladerf_strerror(status));
        return(-1);
    } else {
      bladerf_rx_rate_internal = (*rate);
    }
  }

  return(0);
}

int bladerf_update_tx_bw(void *dev, int *bw) {
  int status, actual;
  
  if ((*bw)==-1) {
    (*bw) = bladerf_tx_bw_internal;
    return(0);
  }

  if ((*bw)!=bladerf_tx_bw_internal) {
    status = bladerf_set_bandwidth((struct bladerf *)dev, BLADERF_MODULE_TX,(*bw),&actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_update_tx_bw: Failed to set tx bandwidth: %s\n", bladerf_strerror(status));
        return(-1);
    } else {
      bladerf_tx_bw_internal = (*bw);
    }
  }

  return(0);
}

int bladerf_update_rx_bw(void *dev, int *bw) {
  int status, actual;
  
  if ((*bw)==-1) {
    (*bw) = bladerf_rx_bw_internal;
    return(0);
  }

  if ((*bw)!=bladerf_rx_bw_internal) {
    status = bladerf_set_bandwidth((struct bladerf *)dev, BLADERF_MODULE_RX,(*bw),&actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_update_rx_bw: Failed to set rx bandwidth: %s\n", bladerf_strerror(status));
        return(-1);
    } else {
      bladerf_rx_bw_internal = (*bw);
    }
  }

  return(0);
}

void *bladerf_rx_task_run(void *tmp)
{
  int status;

  /* Start stream and stay there until we kill the stream */
  status = bladerf_stream(bladerf_rx_stream, BLADERF_MODULE_RX);
  if (status < 0) {
    fprintf(stderr, "RX stream failure: %s\r\n", bladerf_strerror(status));
  }
  return NULL;
}

void bladerf_stop_close_board(void *trx_input){
  struct trx_cfg_op *trx = (struct trx_cfg_op *)trx_input;
  int status;
  struct bladerf *dev = trx->dev;

  fprintf(stderr, "bladerf_stop_close_board...\n");
  
  if (trx->rx.en) {
    pthread_join(bladerf_rx_task, NULL);
    printf("bladerf_stop_close_board: pthread_join(bladerf_rx_task, NULL)\n");
  } 

  if (dev==NULL)
    return;

  if (trx->tx.en) {
    //bladerf_deinit_stream(bladerf_tx_stream);
    //printf("bladerf_stop_close_board: bladerf_deinit_stream(bladerf_tx_stream)\n");

    status = bladerf_enable_module(dev, BLADERF_MODULE_TX, false);
    if (status < 0) {
        fprintf(stderr, "bladerf_stop_close_board: Failed to enable module BLADERF_MODULE_TX: %s\n", bladerf_strerror(status));
    } else {
      fprintf(stdout, "bladerf_stop_close_board: enable module BLADERF_MODULE_TX false: %s\n", bladerf_strerror(status));
    }
  }

  if (trx->rx.en) {
    bladerf_deinit_stream(bladerf_rx_stream);
    printf("bladerf_stop_close_board: bladerf_deinit_stream(bladerf_rx_stream)\n");

    status = bladerf_enable_module(dev, BLADERF_MODULE_RX, false);
    if (status < 0) {
        fprintf(stderr, "bladerf_stop_close_board: Failed to enable module BLADERF_MODULE_RX: %s\n",bladerf_strerror(status));
    } else {
      fprintf(stdout, "bladerf_stop_close_board: enable module BLADERF_MODULE_RX false: %s\n", bladerf_strerror(status));
    }
  }

  bladerf_close(dev);
  printf("bladerf_stop_close_board: bladerf_close.\n");
}

inline int bladerf_tx_one_buf(void *dev, char *buf, int length) {
  int status, i;

  memset( (void *)bladerf_tx_buf, 0, NUM_BLADERF_BUF_SAMPLE_TX*2*sizeof(bladerf_tx_buf[0]) );

  for (i=(NUM_BLADERF_BUF_SAMPLE_TX*2-length); i<(NUM_BLADERF_BUF_SAMPLE_TX*2); i++) {
    bladerf_tx_buf[i] = ( (int)( buf[i-(NUM_BLADERF_BUF_SAMPLE_TX*2-length)] ) )*16;
  }

  // Transmit samples
  status = bladerf_sync_tx(dev, (void *)bladerf_tx_buf, NUM_BLADERF_BUF_SAMPLE_TX, NULL, 10);
  if (status != 0) {
    printf("bladerf_tx_one_buf: Failed to TX samples 1: %s\n",
             bladerf_strerror(status));
    return(-1);
  }

  return(0);
}

inline int bladerf_config_run_board(struct trx_cfg_op *trx) {
  int status;
  unsigned int actual;
  struct bladerf *dev = NULL;

  trx->dev = NULL;

  if (trx->rx.en) {
    bladerf_rx_data.idx = 0;
    bladerf_rx_data.num_buffers = 2;
    bladerf_rx_data.samples_per_buffer = (LEN_BUF/2);
  }

  status = bladerf_open(&dev, NULL);
  if (status < 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to open device: %s\n", bladerf_strerror(status));
      return EXIT_FAILURE;
  } else  {
    fprintf(stdout, "bladerf_config_run_board: open device: %s\n", bladerf_strerror(status));
  }
  
  status = bladerf_is_fpga_configured(dev);
  if (status < 0) {
      fprintf(stderr, "bladerf_config_run_board: Failed to determine FPGA state: %s\n", bladerf_strerror(status));
      return EXIT_FAILURE;
  } else if (status == 0) {
      fprintf(stderr, "bladerf_config_run_board: Error: FPGA is not loaded.\n");
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else  {
    fprintf(stdout, "bladerf_config_run_board: FPGA is loaded.\n");
  }
  
  if (trx->tx.en) {
    status = bladerf_set_frequency(dev, BLADERF_MODULE_TX, trx->tx.freq);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set tx frequency: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
        fprintf(stdout, "bladerf_config_run_board: set tx frequency: %luHz %s\n", trx->tx.freq, bladerf_strerror(status));
    }
  }

  if (trx->rx.en) {
    status = bladerf_set_frequency(dev, BLADERF_MODULE_RX, trx->rx.freq);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set rx frequency: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
        fprintf(stdout, "bladerf_config_run_board: set rx frequency: %luHz %s\n", trx->rx.freq, bladerf_strerror(status));
    }
  }

  if (trx->tx.en){
    status = bladerf_set_sample_rate(dev, BLADERF_MODULE_TX, trx->tx.rate, &actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set tx sample rate: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: set tx sample rate: %dHz %s\n", actual, bladerf_strerror(status));
    }
  }

  if (trx->rx.en){
    status = bladerf_set_sample_rate(dev, BLADERF_MODULE_RX, trx->rx.rate, &actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set rx sample rate: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: set rx sample rate: %dHz %s\n", actual, bladerf_strerror(status));
    }
  }

  if (trx->tx.en) {
    status = bladerf_set_bandwidth(dev, BLADERF_MODULE_TX, trx->tx.bw, &actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set tx bandwidth: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: tx bladerf_set_bandwidth: %d %s\n", actual, bladerf_strerror(status));
    }
  }

  if (trx->rx.en) {
    status = bladerf_set_bandwidth(dev, BLADERF_MODULE_RX, trx->rx.bw, &actual);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set rx bandwidth: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: rx bladerf_set_bandwidth: %d %s\n", actual, bladerf_strerror(status));
    }
  }

  if (trx->tx.en) {
    if (trx->tx.gain!=-1)
      status = bladerf_set_gain(dev, BLADERF_MODULE_TX, trx->tx.gain);
    else
      status = bladerf_set_gain(dev, BLADERF_MODULE_TX, BLADERF_DEFAULT_TX_GAIN);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set tx gain: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: tx bladerf_set_gain: %d %s\n", trx->tx.gain, bladerf_strerror(status));
    }
  }

  if (trx->rx.en) {
    if (trx->rx.gain!=-1)
      status = bladerf_set_gain(dev, BLADERF_MODULE_RX, trx->rx.gain);
    else
      status = bladerf_set_gain(dev, BLADERF_MODULE_RX, BLADERF_DEFAULT_RX_GAIN);
    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to set rx gain: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: rx bladerf_set_gain: %d %s\n", trx->rx.gain, bladerf_strerror(status));
    }
  }

#if 0 // old version do not have this API
  status = bladerf_get_gain(dev, BLADERF_MODULE_RX, &actual);
  if (status != 0) {
      fprintf(stderr, "Failed to get gain: %s\n",
              bladerf_strerror(status));
      bladerf_close(dev);
      return EXIT_FAILURE;
  } else {
    fprintf(stdout, "bladerf_get_gain: %d %s\n", actual,
              bladerf_strerror(status));
  }
#endif

  if (trx->rx.en) {
    /* Initialize the stream */
    status = bladerf_init_stream(
                &bladerf_rx_stream,
                dev,
                bladerf_stream_callback,
                &bladerf_rx_data.buffers,
                bladerf_rx_data.num_buffers,
                BLADERF_FORMAT_SC16_Q11,
                bladerf_rx_data.samples_per_buffer,
                bladerf_rx_data.num_buffers,
                &bladerf_rx_data
              );

    if (status != 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to init rx stream: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: init rx stream: %s\n", bladerf_strerror(status));
    }

    bladerf_set_stream_timeout(dev, BLADERF_MODULE_RX, 100);

    status = bladerf_enable_module(dev, BLADERF_MODULE_RX, true);
    if (status < 0) {
        fprintf(stderr, "bladerf_config_run_board: Failed to enable module: %s\n", bladerf_strerror(status));
        bladerf_deinit_stream(bladerf_rx_stream);
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_config_run_board: enable module true: %s\n", bladerf_strerror(status));
    }

    status = pthread_create(&bladerf_rx_task, NULL, bladerf_rx_task_run, NULL);
    if (status < 0) {
        return EXIT_FAILURE;
    }
  }

  if (trx->tx.en) {
    status = bladerf_sync_config( dev,
                                  BLADERF_MODULE_TX,
                                  BLADERF_FORMAT_SC16_Q11,
                                  32,
                                  NUM_BLADERF_BUF_SAMPLE_TX,
                                  16,
                                  10);

    if (status != 0) {
        fprintf(stderr, "Failed to initialize TX sync handle: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "bladerf_sync_config: %s\n", bladerf_strerror(status));
    }

    status = bladerf_enable_module(dev, BLADERF_MODULE_TX, true);
    if (status < 0) {
        fprintf(stderr, "Failed to enable module: %s\n", bladerf_strerror(status));
        bladerf_close(dev);
        return EXIT_FAILURE;
    } else {
      fprintf(stdout, "enable module true: %s\n", bladerf_strerror(status));
    }
  }

  // set result to instance pointed by trx pointer
  trx->dev = dev;
  trx->hw_type = BLADERF;
  
  if (trx->tx.en) {
    if (trx->tx.gain==-1)
      trx->tx.gain = BLADERF_DEFAULT_TX_GAIN;
    
    trx->tx.update_freq =  bladerf_update_tx_freq;
    trx->tx.update_gain =  bladerf_update_tx_gain;
    trx->tx.update_rate =  bladerf_update_tx_rate;
    trx->tx.update_bw =    bladerf_update_tx_bw;
    trx->tx.proc_one_buf = bladerf_tx_one_buf;
  }

  if (trx->rx.en) {
    if (trx->rx.gain==-1)
      trx->rx.gain = BLADERF_DEFAULT_RX_GAIN;

    trx->rx.update_freq =  bladerf_update_rx_freq;
    trx->rx.update_gain =  bladerf_update_rx_gain;
    trx->rx.update_rate =  bladerf_update_rx_rate;
    trx->rx.update_bw =    bladerf_update_rx_bw;
    trx->rx.proc_one_buf = bladerf_get_rx_sample;
  }

  trx->stop_close = bladerf_stop_close_board;

  return(0);
}

#endif
