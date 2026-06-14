#ifndef _DRM_NOTIFIER_H_
#define _DRM_NOTIFIER_H_

#include <linux/msm_drm_notify.h>

#define DRM_EARLY_EVENT_BLANK   MSM_DRM_EARLY_EVENT_BLANK
#define DRM_EVENT_BLANK         MSM_DRM_EVENT_BLANK
#define DRM_R_EARLY_EVENT_BLANK 0x03
#define DRM_FOD_EVENT           0x04

#define DRM_BLANK_UNBLANK       MSM_DRM_BLANK_UNBLANK
#define DRM_BLANK_LP1           MSM_DRM_BLANK_LP1
#define DRM_BLANK_LP2           MSM_DRM_BLANK_LP2
#define DRM_BLANK_STANDBY       MSM_DRM_BLANK_STANDBY
#define DRM_BLANK_SUSPEND       MSM_DRM_BLANK_SUSPEND
#define DRM_BLANK_POWERDOWN     MSM_DRM_BLANK_POWERDOWN

enum {
        FOD_FINGERDOWN = 0,
        FOD_FINGERUP,
};

struct drm_notify_data {
        bool is_primary;
        void *data;
};

#define drm_register_client(nb)         msm_drm_register_client(nb)
#define drm_unregister_client(nb)       msm_drm_unregister_client(nb)
#define drm_notifier_call_chain(val, v) msm_drm_notifier_call_chain(val, v)

#endif /*_DRM_NOTIFIER_H*/
