#ifndef CTR_VIDEO_H
#define CTR_VIDEO_H

/* Native replacement for ZVideoPlayer's Android Activity. */
void ctr_video_init(void);
void ctr_video_request(const char *name,int flag1,int flag2);
void ctr_video_update(void);
void ctr_video_render(void);
int ctr_video_is_active(void);
void ctr_video_request_skip(void);
int ctr_video_take_completion(void);
void ctr_video_shutdown(void);

#endif
