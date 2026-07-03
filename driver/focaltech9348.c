/*
 * FocalTech FT9348 (2808:9348, chip 0x9362) image driver for libfprint.
 * Reverse-engineered from ftWbioUmdfDriverV2.dll (Windows) + hardware validation.
 *
 * Малый сенсор 64x80 (~3x4мм): один кадр = ~8 гребней, для minutiae мало.
 * Поэтому СВАЙП: снимаем поток кадров при движении пальца и сшиваем (fpi_assemble_frames)
 * в большое изображение, как делают swipe-драйверы (elan/aesx660).
 *
 * Протокол (control/register):
 *   read reg : control IN  0xC0 vendor/dev, bRequest=reg
 *   write reg: control OUT 0x40 vendor/dev, bRequest=reg, wValue, wIndex
 *   finger   : read reg 0x43 -> 0x01 = палец
 *   wake     : wr 0x34=0xff; 0x34=2; 0x6f(wIndex=0xff00)
 *   mode     : wr 0x3b(1, idx=0x1f); 0x3b(1, idx=0x1e); rd 0x3a(idx=0x1d)
 *   кадр     : wr 0x34=0xff; 0x34=3; 0x6f(wIndex=0x9080, wValue=5120); bulk-IN EP 0x83
 *
 * LGPL 2.1+.
 */

#define FP_COMPONENT "focaltech9348"
#include "fpi-log.h"
#include "drivers_api.h"
#include "fpi-assembling.h"

#define FT_WIDTH  64
#define FT_HEIGHT 80
#define FT_FRAME_SIZE (FT_WIDTH * FT_HEIGHT)
#define FT_EP_IN (0x83 | FPI_USB_ENDPOINT_IN)
#define FT_FRAME_ADDR 0x9080
#define FT_TIMEOUT 1000
#define FT_POLL_DELAY 60
#define FT_FRAME_DELAY 25    /* пауза между кадрами свайпа (мс) */
#define FT_CAP_FRAMES 30     /* сколько кадров снять за свайп */
#define FT_MIN_FRAMES 5
#define FT_NORM_RADIUS 4

/* get_pixel для сборки */
static unsigned char
ft_get_pixel (struct fpi_frame_asmbl_ctx *ctx, struct fpi_frame *frame,
              unsigned int x, unsigned int y)
{
  return frame->data[x + y * ctx->frame_width];
}

static struct fpi_frame_asmbl_ctx ft_asmbl_ctx = {
  .frame_width = FT_WIDTH,
  .frame_height = FT_HEIGHT,
  .image_width = FT_WIDTH * 3 / 2,   /* запас по ширине под дрейф пальца */
  .get_pixel = ft_get_pixel,
};

/* SSM states */
enum {
  S_POLL_DELAY,                     /* пауза между опросами пальца */
  S_WAIT_FINGER,                    /* poll 0x43 до касания (ДО wake!) */
  S_WAKE_A, S_WAKE_B, S_WAKE_C,     /* разбудить устройство */
  S_MODE_A, S_MODE_B, S_MODE_RD,    /* режим съёмки */
  S_RD_A, S_RD_B, S_RD_C, S_READ,   /* один кадр */
  S_STORE,                          /* сохранить кадр (+ пауза) */
  S_LOOP,                           /* ещё кадр или сборка */
  S_ASSEMBLE,
  S_NUM_STATES,
};

struct _FpiDeviceFocaltech9348
{
  FpImageDevice parent;
  FpiSsm       *ssm;
  guint8       *frame;      /* сырой кадр */
  GSList       *frames;     /* собранные fpi_frame (нормализованные) */
  int           num_frames;
  gboolean      deactivating;
};
G_DECLARE_FINAL_TYPE (FpiDeviceFocaltech9348, fpi_device_focaltech9348, FPI, DEVICE_FOCALTECH9348, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceFocaltech9348, fpi_device_focaltech9348, FP_TYPE_IMAGE_DEVICE);

/* --- USB helpers --- */

static void
ft_write_reg (FpiDeviceFocaltech9348 *self, guint8 reg, guint16 value, guint16 idx)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (FP_DEVICE (self));

  t->ssm = self->ssm;
  fpi_usb_transfer_fill_control (t, G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE,
                                 reg, value, idx, 0);
  fpi_usb_transfer_submit (t, FT_TIMEOUT, fpi_device_get_cancellable (FP_DEVICE (self)),
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
ft_read_ignore (FpiDeviceFocaltech9348 *self, guint8 reg, guint16 value, guint16 idx, gsize len)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (FP_DEVICE (self));

  t->ssm = self->ssm;
  fpi_usb_transfer_fill_control (t, G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE,
                                 reg, value, idx, len);
  fpi_usb_transfer_submit (t, FT_TIMEOUT, fpi_device_get_cancellable (FP_DEVICE (self)),
                           fpi_ssm_usb_transfer_cb, NULL);
}

/* poll finger в S_WAIT_FINGER: если палец -> начать wake, иначе ждать */
static void
ft_wait_finger_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer u, GError *error)
{
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (dev);

  if (error) { fpi_ssm_mark_failed (transfer->ssm, error); return; }
  if (self->deactivating) { fpi_ssm_mark_completed (transfer->ssm); return; }

  if (transfer->buffer[0] == 0x01)
    {
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), TRUE);
      fpi_ssm_next_state (transfer->ssm);   /* палец есть -> S_WAKE_A -> ... -> захват */
    }
  else
    fpi_ssm_jump_to_state (transfer->ssm, S_POLL_DELAY);  /* ждать дальше (с паузой) */
}

static void
ft_poll_finger (FpiDeviceFocaltech9348 *self, FpiUsbTransferCallback cb)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (FP_DEVICE (self));

  t->ssm = self->ssm;
  fpi_usb_transfer_fill_control (t, G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE,
                                 0x43, 0, 0, 1);
  fpi_usb_transfer_submit (t, FT_TIMEOUT, fpi_device_get_cancellable (FP_DEVICE (self)), cb, NULL);
}

/* нормализовать сырой кадр self->frame -> store как fpi_frame */
static void
ft_store_frame (FpiDeviceFocaltech9348 *self)
{
  struct fpi_frame *fr = g_malloc0 (FT_FRAME_SIZE + sizeof (struct fpi_frame));
  guint8 *src = self->frame;
  int x, y;

  for (y = 0; y < FT_HEIGHT; y++)
    for (x = 0; x < FT_WIDTH; x++)
      {
        int x0 = MAX (0, x - FT_NORM_RADIUS), x1 = MIN (FT_WIDTH - 1, x + FT_NORM_RADIUS);
        int y0 = MAX (0, y - FT_NORM_RADIUS), y1 = MIN (FT_HEIGHT - 1, y + FT_NORM_RADIUS);
        long sum = 0; int n = 0, ix, iy;
        for (iy = y0; iy <= y1; iy++)
          for (ix = x0; ix <= x1; ix++)
            { sum += src[iy * FT_WIDTH + ix]; n++; }
        float mean = (float) sum / n;
        float v = 128.0f + 3.0f * ((float) src[y * FT_WIDTH + x] - mean);
        fr->data[y * FT_WIDTH + x] = v < 0 ? 0 : (v > 255 ? 255 : (guint8) v);
      }
  self->frames = g_slist_prepend (self->frames, fr);
  self->num_frames++;
}

static void
ft_read_frame_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer u, GError *error)
{
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (dev);

  if (error) { fpi_ssm_mark_failed (transfer->ssm, error); return; }
  memcpy (self->frame, transfer->buffer, MIN (transfer->actual_length, FT_FRAME_SIZE));
  fpi_ssm_next_state (transfer->ssm);   /* -> S_STORE_CHECK */
}

static void
ft_read_frame (FpiDeviceFocaltech9348 *self)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (FP_DEVICE (self));

  t->ssm = self->ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (t, FT_EP_IN, FT_FRAME_SIZE);
  fpi_usb_transfer_submit (t, FT_TIMEOUT, fpi_device_get_cancellable (FP_DEVICE (self)),
                           ft_read_frame_cb, NULL);
}

static void
ft_assemble_submit (FpiDeviceFocaltech9348 *self, FpImageDevice *dev)
{
  FpImage *img;

  self->frames = g_slist_reverse (self->frames);
  fpi_do_movement_estimation (&ft_asmbl_ctx, self->frames);
  img = fpi_assemble_frames (&ft_asmbl_ctx, self->frames);
  fp_dbg ("FTASM frames=%d assembled %ux%u", self->num_frames, img->width, img->height);
  img->flags |= FPI_IMAGE_PARTIAL;
  img->ppmm = 19.685;   /* 500 DPI */

  g_slist_free_full (self->frames, g_free);
  self->frames = NULL;
  self->num_frames = 0;

  fpi_image_device_image_captured (dev, img);
  fpi_image_device_report_finger_status (dev, FALSE);
}

/* --- SSM --- */

static void
ft_loop_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (_dev);

  if (self->deactivating) { fpi_ssm_mark_completed (ssm); return; }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case S_POLL_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT_POLL_DELAY);   /* -> S_WAIT_FINGER */
      break;

    case S_WAIT_FINGER:
      ft_poll_finger (self, ft_wait_finger_cb);
      break;

    case S_WAKE_A: ft_write_reg (self, 0x34, 0xff, 0); break;
    case S_WAKE_B: ft_write_reg (self, 0x34, 0x02, 0); break;
    case S_WAKE_C: ft_write_reg (self, 0x6f, 0x0000, 0xff00); break;
    case S_MODE_A: ft_write_reg (self, 0x3b, 0x0001, 0x001f); break;
    case S_MODE_B: ft_write_reg (self, 0x3b, 0x0001, 0x001e); break;
    case S_MODE_RD: ft_read_ignore (self, 0x3a, 0, 0x001d, 1); break;

    case S_RD_A: ft_write_reg (self, 0x34, 0xff, 0); break;
    case S_RD_B: ft_write_reg (self, 0x34, 0x03, 0); break;
    case S_RD_C: ft_write_reg (self, 0x6f, FT_FRAME_SIZE, FT_FRAME_ADDR); break;
    case S_READ: ft_read_frame (self); break;

    case S_STORE:
      ft_store_frame (self);
      fpi_ssm_next_state_delayed (ssm, FT_FRAME_DELAY);   /* пауза для движения пальца -> S_LOOP */
      break;

    case S_LOOP:
      if (self->num_frames < FT_CAP_FRAMES)
        fpi_ssm_jump_to_state (ssm, S_RD_A);   /* следующий кадр */
      else
        fpi_ssm_next_state (ssm);              /* -> S_ASSEMBLE */
      break;

    case S_ASSEMBLE:
      if (self->num_frames >= FT_MIN_FRAMES)
        {
          ft_assemble_submit (self, dev);
          fpi_ssm_mark_completed (ssm);
        }
      else
        {
          /* слишком короткий свайп — сбросить и ждать заново */
          fp_dbg ("swipe too short: %d frames (need %d)", self->num_frames, FT_MIN_FRAMES);
          g_slist_free_full (self->frames, g_free);
          self->frames = NULL;
          self->num_frames = 0;
          fpi_image_device_report_finger_status (dev, FALSE);
          fpi_ssm_jump_to_state (ssm, S_WAIT_FINGER);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
ft_loop_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (_dev);

  self->ssm = NULL;
  g_slist_free_full (self->frames, g_free);
  self->frames = NULL;
  self->num_frames = 0;

  if (self->deactivating)
    fpi_image_device_deactivate_complete (dev, error);
  else if (error != NULL)
    fpi_image_device_session_error (dev, error);
}

/* --- image device vfuncs --- */

static void
ft_dev_init (FpImageDevice *dev)
{
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (dev);
  GError *error = NULL;

  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);
  self->frame = g_malloc0 (FT_FRAME_SIZE);
  fpi_image_device_open_complete (dev, error);
}

static void
ft_dev_deinit (FpImageDevice *dev)
{
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (dev);
  GError *error = NULL;

  g_clear_pointer (&self->frame, g_free);
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
ft_dev_activate (FpImageDevice *dev)
{
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (dev);

  self->deactivating = FALSE;
  fpi_image_device_activate_complete (dev, NULL);
}

static void
ft_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (dev);

  if (self->ssm)
    self->deactivating = TRUE;
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

static void
ft_dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceFocaltech9348 *self = FPI_DEVICE_FOCALTECH9348 (dev);

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      self->num_frames = 0;
      self->frames = NULL;
      self->ssm = fpi_ssm_new (FP_DEVICE (dev), ft_loop_state, S_NUM_STATES);
      fpi_ssm_start (self->ssm, ft_loop_complete);
    }
}

static const FpIdEntry id_table[] = {
  { .vid = 0x2808, .pid = 0x9348, },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_focaltech9348_init (FpiDeviceFocaltech9348 *self)
{
}

static void
fpi_device_focaltech9348_class_init (FpiDeviceFocaltech9348Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "FocalTech FT9348 Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  img_class->img_width = FT_WIDTH * 3 / 2;
  img_class->img_height = -1;   /* переменная высота (свайп) */
  img_class->bz3_threshold = 20;

  img_class->img_open = ft_dev_init;
  img_class->img_close = ft_dev_deinit;
  img_class->activate = ft_dev_activate;
  img_class->deactivate = ft_dev_deactivate;
  img_class->change_state = ft_dev_change_state;
}
