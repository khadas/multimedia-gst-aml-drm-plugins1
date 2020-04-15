#ifndef _GST_VP9_SEC_TRANS_H_
#define _GST_VP9_SEC_TRANS_H_

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_VP9_SEC_TRANS \
  (gst_vp9_sec_trans_get_type())
#define GST_VP9_SEC_TRANS(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VP9_SEC_TRANS,GstVp9SecTrans))
#define GST_VP9_SEC_TRANS_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VP9_SEC_TRANS,GstVp9SecTransClass))
  #define GST_VP9_SEC_TRANS_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_VP9_SEC_TRANS,GstVp9SecTransClass))
#define GST_IS_VP9_SEC_TRANS(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VP9_SEC_TRANS))
#define GST_IS_VP9_SEC_TRANS_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VP9_SEC_TRANS))


typedef struct _GstVp9SecTrans      GstVp9SecTrans;
typedef struct _GstVp9SecTransClass GstVp9SecTransClass;


struct _GstVp9SecTrans
{
    GstBaseTransform        element;
};

struct _GstVp9SecTransClass {
    GstBaseTransformClass   parent_class;
};


GType gst_vp9_sec_trans_get_type (void);

G_END_DECLS
#endif /* _GST_VP9_SEC_TRANS_H_ */
