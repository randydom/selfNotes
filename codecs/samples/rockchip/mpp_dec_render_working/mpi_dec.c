/*
   Copyright (c) 2017, Martin Cerveny

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.
 * Neither the name of the copyright holder nor the
 names of its contributors may be used to endorse or promote products
 derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define MODULE_TAG "mpi_dec"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <rk_mpi.h>


#define READ_BUF_SIZE (SZ_1M)
#define MAX_FRAMES 24		// min 16 and 20+ recommended (mpp/readme.txt) ?
#define FPS 25
//#define RAW_DUMP_TO_FILE 1

struct {
  int fd;

  uint32_t plane_id, crtc_id;

  int frm_eos;

  int crtc_width;
  int crtc_height;
  RK_U32 frm_width;
  RK_U32 frm_height;
  int fb_x, fb_y, fb_width, fb_height;

  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int fb_id;
  int skipped_frames;
} drm;

struct {
  MppCtx          ctx;
  MppApi          *mpi;

  struct timespec first_frame_ts;

  MppBufferGroup	frm_grp;
  struct {
    int prime_fd;
    int fb_id;
    uint32_t handle;
  } frame_to_drm[MAX_FRAMES];
} mpi;


#if RAW_DUMP_TO_FILE
int dump_mpp_frame_to_file(MppFrame frame, FILE *fp)
{
    RK_U32 width = 0;
    RK_U32 height = 0;
    RK_U32 h_stride = 0;
    RK_U32 v_stride = 0;
    MppFrameFormat fmt = MPP_FMT_YUV420SP;
    MppBuffer buffer = NULL;
    RK_U8 *base = NULL;

    if (!fp || !frame)
        return -1;

    width = mpp_frame_get_width(frame);
    height = mpp_frame_get_height(frame);
    h_stride = mpp_frame_get_hor_stride(frame);
    v_stride = mpp_frame_get_ver_stride(frame);
    fmt = mpp_frame_get_fmt(frame);
    buffer = mpp_frame_get_buffer(frame);

    if(!buffer)
        return -2;

    base = (RK_U8 *)mpp_buffer_get_ptr(buffer);
    {
        RK_U32 i;
        RK_U8 *base_y = base;
        RK_U8 *base_u = base + h_stride * v_stride;
        RK_U8 *base_v = base_u + h_stride * v_stride / 4;

        for (i = 0; i < height; i++, base_y += h_stride)
            fwrite(base_y, 1, width, fp);
        for (i = 0; i < height / 2; i++, base_u += h_stride / 2)
            fwrite(base_u, 1, width / 2, fp);
        for (i = 0; i < height / 2; i++, base_v += h_stride / 2)
            fwrite(base_v, 1, width / 2, fp);
    }

    return 0;
}
#endif


// frame_thread
//
// - allocate DRM buffers and DRM FB based on frame size 
// - pickup frame in blocking mode and output to screen overlay

void *frame_thread(void *param)
{
  int ret;
  int i;    
  MppFrame  frame  = NULL;
  int frid = 0;
  int frm_eos = 0;

#if RAW_DUMP_TO_FILE
  FILE *mFout = fopen("rawdump.yuv", "wb+");
  if (!mFout) {
    printf("failed to open output file .\n");
    return NULL;
  }
#endif
  
  static int naseeb_count = 0;
  printf("FRAME THREAD START\n");
  while (!drm.frm_eos) {
    struct timespec ts, ats;

    assert(!frame);
    ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
    assert(!ret);
    clock_gettime(CLOCK_MONOTONIC, &ats);

#if 0
    if(naseeb_count == 0)
    {
      printf("***naseeb after decoder_get_frame\n");
      MppBuffer buffer_temp = mpp_frame_get_buffer(frame);					
      if (buffer_temp) {
        printf("***naseeb after decoder_get_frame\n");
      }
      else
      {
        printf("***naseeb no buffer\n");
      }
      naseeb_count++;
    }
#endif

#if RAW_DUMP_TO_FILE
    dump_mpp_frame_to_file(frame, mFout);
#endif
    //printf("GETFRAME %3d.%06d frid %d\n", ats.tv_sec, ats.tv_nsec/1000, frid);
    if (frame) {
      if (mpp_frame_get_info_change(frame)) {
printf("****naseeb Frame info changed\n");
        // new resolution
        assert(!mpi.frm_grp);

        drm.frm_width = mpp_frame_get_width(frame);
        drm.frm_height = mpp_frame_get_height(frame);
        RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
        RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
        MppFrameFormat fmt = mpp_frame_get_fmt(frame);
        assert(fmt == MPP_FMT_YUV420SP);	// only supported for testing MPP_FMT_YUV420SP == DRM_FORMAT_NV12

        printf("frame changed %d(%d)x%d(%d)\n", drm.frm_width, hor_stride, drm.frm_height, ver_stride);
#if 0				
        // position overlay, expand to full screen
        drm.fb_x = 0;
        drm.fb_y = 0;
        drm.fb_width = drm.crtc_width;
        drm.fb_height = drm.crtc_height;
#else				
        // position overlay, scale to ratio
        float crt_ratio = (float)drm.crtc_width/drm.crtc_height;
        float frame_ratio = (float)drm.frm_width/drm.frm_height;

        if (crt_ratio>frame_ratio) {
          drm.fb_width = frame_ratio/crt_ratio*drm.crtc_width;
          drm.fb_height = drm.crtc_height;
          drm.fb_x = (drm.crtc_width-drm.fb_width)/2;
          drm.fb_y = 0;
        }
        else {
          drm.fb_width = drm.crtc_width;
          drm.fb_height = crt_ratio/frame_ratio*drm.crtc_height;
          drm.fb_x = 0;
          drm.fb_y = (drm.crtc_height-drm.fb_height)/2;
        }
#endif				
        // create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
        assert(!mpi.frm_grp);
        ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
        assert(!ret);                    
        for (i=0; i<MAX_FRAMES; i++) {

          // new DRM buffer
          struct drm_mode_create_dumb dmcd;
          memset(&dmcd, 0, sizeof(dmcd));
          dmcd.bpp = 8;
          dmcd.width = hor_stride;
          dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
          do {
            ret = ioctl(drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
          } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
          assert(!ret);
printf("***naseeb dmcd.pitch:%d\n", dmcd.pitch);
          //assert(dmcd.pitch==hor_stride);
          //assert(dmcd.size==hor_stride*ver_stride*2);
          mpi.frame_to_drm[i].handle = dmcd.handle;

          // commit DRM buffer to frame group 
          struct drm_prime_handle dph;
          memset(&dph, 0, sizeof(struct drm_prime_handle));
          dph.handle = dmcd.handle;
          dph.fd = -1;
          do {
            ret = ioctl(drm.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
          } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
          assert(!ret);
          MppBufferInfo info;
          memset(&info, 0, sizeof(info));
          info.type = MPP_BUFFER_TYPE_DRM;
          info.size = dmcd.width*dmcd.height;
          info.fd = dph.fd;
          ret = mpp_buffer_commit(mpi.frm_grp, &info);
          assert(!ret);
          mpi.frame_to_drm[i].prime_fd = info.fd; // dups fd						
          if (dph.fd != info.fd) {
            ret = close(dph.fd);
            assert(!ret);
          }

          // allocate DRM FB from DRM buffer 
          uint32_t handles[4], pitches[4], offsets[4];
          memset(handles, 0, sizeof(handles));
          memset(pitches, 0, sizeof(pitches));
          memset(offsets, 0, sizeof(offsets));
          handles[0] = mpi.frame_to_drm[i].handle;
          offsets[0] = 0;
          pitches[0] = hor_stride;						
          handles[1] = mpi.frame_to_drm[i].handle;
          offsets[1] = hor_stride * ver_stride;
          pitches[1] = hor_stride;
          ret = drmModeAddFB2(drm.fd, drm.frm_width, drm.frm_height, DRM_FORMAT_NV12, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
          assert(!ret);
        }
        // register external frame group
        ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
        ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

      } else {
        // regular framer received
        if (!mpi.first_frame_ts.tv_sec) {
          ts = ats;
          mpi.first_frame_ts = ats;
        }

        MppBuffer buffer = mpp_frame_get_buffer(frame);					
        if (buffer) {
          // find fb_id by frame prime_fd
          MppBufferInfo info;
          ret = mpp_buffer_info_get(buffer, &info);
          assert(!ret);
          for (i=0; i<MAX_FRAMES; i++) {
            if (mpi.frame_to_drm[i].prime_fd == info.fd) break;
          }
          assert(i!=MAX_FRAMES);
#if 0
          printf("FRAME %d.%06d first %8jd previous %6jd frid %d\n", ats.tv_sec, ats.tv_nsec/1000, 
              (ats.tv_sec - mpi.first_frame_ts.tv_sec)*1000000ll + ((ats.tv_nsec - mpi.first_frame_ts.tv_nsec)/1000ll) % 1000000ll,
              (ats.tv_sec - ts.tv_sec)*1000000ll + ((ats.tv_nsec - ts.tv_nsec)/1000ll) % 1000000ll, frid);
#endif
          ts = ats;
          frid++;

          // send DRM FB to display thread
          ret = pthread_mutex_lock(&drm.mutex);
          assert(!ret);
          if (drm.fb_id) drm.skipped_frames++;
          drm.fb_id = mpi.frame_to_drm[i].fb_id;
printf("****naseeb received frame on index: %d\n", i);
//		usleep(25000);
          ret = pthread_cond_signal(&drm.cond);
          assert(!ret);
          ret = pthread_mutex_unlock(&drm.mutex);
          assert(!ret);

        } else printf("FEAME no buff\n");
      }

      drm.frm_eos = mpp_frame_get_eos(frame);
      mpp_frame_deinit(&frame);
      frame = NULL;
    } else assert(0);
  } 
  printf("FRAME THREAD END\n");
  return NULL;
}

// frame_thread
//
// wait and display last DRM FB (some may be skipped)

void *display_thread(void *param)
{
  int ret;    
  printf("DISPLAY THREAD START\n");

  while (!drm.frm_eos) {
    int fb_id;

    ret = pthread_mutex_lock(&drm.mutex);
    assert(!ret);
    while (drm.fb_id==0) {
      pthread_cond_wait(&drm.cond, &drm.mutex);
      assert(!ret);
      if (drm.fb_id == 0 && drm.frm_eos) {
        ret = pthread_mutex_unlock(&drm.mutex);
        assert(!ret);
        goto end;
      }
    }	
    fb_id = drm.fb_id;
    if (drm.skipped_frames) printf("DISPLAY skipped %d\n", drm.skipped_frames);
    drm.fb_id=0;
    drm.skipped_frames=0;
    ret = pthread_mutex_unlock(&drm.mutex);
    assert(!ret);

    // show DRM FB in overlay plane (auto vsynced/atomic !)
    ret = drmModeSetPlane(drm.fd, drm.plane_id, drm.crtc_id, fb_id, 0,
        drm.fb_x, drm.fb_y, drm.fb_width, drm.fb_height, 
        0, 0, drm.frm_width << 16, drm.frm_height << 16);
    assert(!ret);
		printf("***naseeb sleeping as per FPS\n");
  }
end:	
  printf("DISPLAY THREAD END\n");
}

// main

int main(int argc, char **argv)
{
  int ret;	
  int i, j;

  ////////////////////////////////// PARAMETER SETUP

  if (argc != 3) {
    printf("usage: %s raw_filename mpp_coding_id\n\n", argv[0]);
    mpp_show_support_format();
    return 0;
  }

  int data_fd=open(argv[1], O_RDONLY);
  assert(data_fd > 0);

  MppCodingType mpp_type = (MppCodingType)atoi(argv[2]);
  ret = mpp_check_support_format(MPP_CTX_DEC, mpp_type);
  assert(!ret);
  // MPP_VIDEO_CodingMJPEG only in advanced mode
  assert(mpp_type != MPP_VIDEO_CodingMJPEG); 

  //////////////////////////////////  DRM SETUP

  drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  assert(drm.fd >= 0);

  drmModeRes *resources = drmModeGetResources(drm.fd);
  assert(resources);

  // find active monitor
  drmModeConnector *connector;
  for (i = 0; i < resources->count_connectors; ++i) {
    connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
    if (!connector)
      continue;
    if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
      printf("CONNECTOR type: %d id: %d\n", connector->connector_type, connector->connector_id);
      break;
    }
    drmModeFreeConnector(connector);
  }
  assert(i < resources->count_connectors);

  drmModeEncoder *encoder;	
  for (i = 0; i < resources->count_encoders; ++i) {
    encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
    if (!encoder)
      continue;
    if (encoder->encoder_id == connector->encoder_id) {
      printf("ENCODER id: %d\n", encoder->encoder_id);
      break;
    }
    drmModeFreeEncoder(encoder);
  }
  assert(i < resources->count_encoders);

  drmModeCrtcPtr crtc;	
  for (i = 0; i < resources->count_crtcs; ++i) {
    if (resources->crtcs[i] == encoder->crtc_id) {
      crtc = drmModeGetCrtc(drm.fd, resources->crtcs[i]);
      assert(crtc);
      break;
    }
  }
  assert(i < resources->count_crtcs);
  drm.crtc_id = crtc->crtc_id;
  drm.crtc_width = crtc->width;
  drm.crtc_height = crtc->height;
  uint32_t crtc_bit = (1 << i);

  ret = drmSetClientCap(drm.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1); // not needed (overlays, primary, cursor)
  assert(!ret);
  drmModePlaneRes *plane_resources =  drmModeGetPlaneResources(drm.fd);
  assert(plane_resources);

  drmModePlane *ovr;

  // search for OVERLAY (for active conector, unused, NV12 support)
  for (i = 0; i < plane_resources->count_planes; i++) {		
    ovr = drmModeGetPlane(drm.fd, plane_resources->planes[i]);
    if (!ovr)
      continue;

    for (j=0; j<ovr->count_formats; j++) 
      if (ovr->formats[j] ==  DRM_FORMAT_NV12) break;
    if (j==ovr->count_formats)
      continue;

    if ((ovr->possible_crtcs & crtc_bit) && !ovr->crtc_id) {
      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm.fd, plane_resources->planes[i], DRM_MODE_OBJECT_PLANE);
      if (!props) continue;

      for (j = 0; j < props->count_props && !drm.plane_id; j++) {
        drmModePropertyPtr prop = drmModeGetProperty(drm.fd, props->props[j]);
        if (!prop) continue;
        if (!strcmp(prop->name, "type") && props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY) {
          drm.plane_id = ovr->plane_id;
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
      if (drm.plane_id) break;
    }
    drmModeFreePlane(ovr);
  }
  assert(drm.plane_id);

  ////////////////////////////////////////////// MPI SETUP

  MppPacket packet;
  void *pkt_buf = malloc(READ_BUF_SIZE);
  assert(pkt_buf);
  ret = mpp_packet_init(&packet, pkt_buf, READ_BUF_SIZE);
  assert(!ret);

  ret = mpp_create(&mpi.ctx, &mpi.mpi);
  assert(!ret);

  // decoder split mode (multi-data-input) need to be set before init
  int param = 1;
  ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &param);
  assert(!ret);

  //mpp_env_set_u32("mpi_debug", 0x1);
  //mpp_env_set_u32("mpp_buffer_debug", 0xf);
  //mpp_env_set_u32("h265d_debug", 0xfff);

  ret = mpp_init(mpi.ctx, MPP_CTX_DEC, mpp_type);
  assert(!ret);


  // blocked/wait read of frame in thread 
  param = MPP_POLL_BLOCK;
  ret = mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_BLOCK, &param);
  assert(!ret);

  ret = pthread_mutex_init(&drm.mutex, NULL);
  assert(!ret);
  ret = pthread_cond_init(&drm.cond, NULL);
  assert(!ret);

  pthread_t tid_frame, tid_display;
  ret = pthread_create(&tid_frame, NULL, frame_thread, NULL);
  assert(!ret);
  ret = pthread_create(&tid_display, NULL, display_thread, NULL);
  assert(!ret);

  ////////////////////////////////////////////// MAIN LOOP

  while (1) {
    do {
      ret=read(data_fd, pkt_buf, READ_BUF_SIZE);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    assert(ret>=0);
    int read_size = ret;
    static RK_S64 pts, dts;
    if (read_size) {
      mpp_packet_set_pos(packet, pkt_buf);
      mpp_packet_set_length(packet, read_size);

      while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
        // buffer 4 packet is hardcoded (actual is MPP_DEC_GET_STREAM_COUNT) and does not support blocking write 
        usleep(10000);
      }

#if 0
      RK_U32 s_cnt, v_cnt; 
      ret = mpi.mpi->control(mpi.ctx, MPP_DEC_GET_STREAM_COUNT, &s_cnt);
      assert(!ret);			
      ret = mpi.mpi->control(mpi.ctx, MPP_DEC_GET_VPUMEM_USED_COUNT, &v_cnt);
      assert(!ret);	
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);	
      printf("PACKET SEND %d.%06d S %d V %d\n", ts.tv_sec, ts.tv_nsec/1000, s_cnt, v_cnt);	
#endif
    }
    else {
      printf("PACKET EOS\n");
      mpp_packet_set_eos(packet);
      mpp_packet_set_pos(packet, pkt_buf);
      mpp_packet_set_length(packet, 0);
      while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
        usleep(10000);
      }
      break;
    }
  }
  close(data_fd);

  ////////////////////////////////////////////// MPI CLEANUP

  ret = pthread_join(tid_frame, NULL);
  assert(!ret);

  ret = pthread_mutex_lock(&drm.mutex);
  assert(!ret);	
  ret = pthread_cond_signal(&drm.cond);
  assert(!ret);	
  ret = pthread_mutex_unlock(&drm.mutex);
  assert(!ret);	

  ret = pthread_join(tid_display, NULL);
  assert(!ret);	

  ret = pthread_cond_destroy(&drm.cond);
  assert(!ret);
  ret = pthread_mutex_destroy(&drm.mutex);
  assert(!ret);

  ret = mpi.mpi->reset(mpi.ctx);
  assert(!ret);

  if (mpi.frm_grp) {
    ret = mpp_buffer_group_put(mpi.frm_grp);
    assert(!ret);
    mpi.frm_grp = NULL;
    for (i=0; i<MAX_FRAMES; i++) {
      ret = drmModeRmFB(drm.fd, mpi.frame_to_drm[i].fb_id);
      assert(!ret);
      struct drm_mode_destroy_dumb dmdd;
      memset(&dmdd, 0, sizeof(dmdd));
      dmdd.handle = mpi.frame_to_drm[i].handle;
      do {
        ret = ioctl(drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdd);
      } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
      assert(!ret);
    }
  }

  mpp_packet_deinit(&packet);
  mpp_destroy(mpi.ctx);
  free(pkt_buf);

  ////////////////////////////////////////////// DRM CLEANUP

  drmModeFreePlane(ovr);
  drmModeFreePlaneResources(plane_resources);
  drmModeFreeEncoder(encoder);
  drmModeFreeConnector(connector);
  drmModeFreeCrtc(crtc);
  drmModeFreeResources(resources);
  close(drm.fd);

  return 0;
}

