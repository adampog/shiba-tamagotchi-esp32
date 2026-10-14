// USB gamepad input for the 8BitDo SN30 Pro (and similar) plugged into the board's USB-C port.
//
// Two paths, whichever the controller enumerates as:
//  - HID (DirectInput / generic gamepad): decoded by EspUsbHost's onGamepad() (axes, hat, buttons).
//  - XInput (Xbox 360 protocol, vendor class 0xFF/0x5D/0x01): EspUsbHost only opens bulk vendor
//    endpoints, so a second usb_host client here claims the interface and reads its interrupt IN
//    endpoint directly. Report layout: [2] dpad/start/back bits, [3] LB/RB/guide/A/B/X/Y bits,
//    [6..7] left stick X (int16 LE).
#include "gamepad.h"
#include <Arduino.h>
#include "EspUsbHost.h"
#include "usb/usb_host.h"

static EspUsbHost usb;
static portMUX_TYPE padMux = portMUX_INITIALIZER_UNLOCKED;
static PadState state = {};

static void setState(const PadState &s) {
  portENTER_CRITICAL(&padMux);
  state = s;
  portEXIT_CRITICAL(&padMux);
}

PadState padRead() {
  portENTER_CRITICAL(&padMux);
  PadState s = state;
  portEXIT_CRITICAL(&padMux);
  return s;
}

// ---------- HID gamepads (via EspUsbHost) ----------
static int hidAddress = -1;

static void onHidGamepad(const EspUsbHostGamepadEvent &ev) {
  PadState s = {};
  s.connected = true;
  int32_t hat = -1, hatMin = 0, hatMax = 7;
  for (size_t i = 0; i < ev.fieldCount; i++) {
    const EspUsbHostHIDFieldValue &f = ev.fields[i];
    if (f.usagePage == 0x01 && f.usage == 0x30) {  // X axis -> -127..127
      int32_t range = f.logicalMax - f.logicalMin;
      if (range > 0) s.lx = (int8_t)constrain((f.value - f.logicalMin) * 254 / range - 127, -127, 127);
    } else if (f.usagePage == 0x01 && f.usage == 0x39) {  // hat switch
      hat = f.value; hatMin = f.logicalMin; hatMax = f.logicalMax;
    } else if (f.usagePage == 0x01 && f.usage >= 0x90 && f.usage <= 0x93 && f.value) {  // d-pad as buttons
      if (f.usage == 0x90) s.up = true;
      if (f.usage == 0x91) s.down = true;
      if (f.usage == 0x92) s.right = true;
      if (f.usage == 0x93) s.left = true;
    } else if (f.usagePage == 0x09 && f.value) {  // buttons
      // 8BitDo D-input order: 1 B, 2 A, 4 Y, 5 X ... 11 select, 12 start (unverified on every
      // firmware, so A/B and X/Y are both accepted as "action" in the game)
      switch (f.usage) {
        case 1: s.b = true; break;
        case 2: s.a = true; break;
        case 4: s.y = true; break;
        case 5: s.x = true; break;
        case 11: s.select = true; break;
        case 12: s.start = true; break;
        case 10: s.start = true; break;  // some modes put START on 10
        case 9: s.select = true; break;
        default: break;
      }
    }
  }
  if (hat >= hatMin && hat <= hatMax) {  // 0=N, 1=NE, 2=E ... 7=NW
    int h = hat - hatMin;
    s.up = s.up || h == 0 || h == 1 || h == 7;
    s.right = s.right || (h >= 1 && h <= 3);
    s.down = s.down || (h >= 3 && h <= 5);
    s.left = s.left || (h >= 5 && h <= 7);
  }
  hidAddress = ev.address;
  setState(s);
}

// ---------- XInput (own usb_host client) ----------
static usb_host_client_handle_t xClient = nullptr;
static usb_device_handle_t xDev = nullptr;
static usb_transfer_t *xXfer = nullptr;
static uint8_t xIntf = 0xFF, xAddr = 0;
static volatile bool xGone = false;

static void xinputParse(const uint8_t *d, int n) {
  if (n < 10 || d[0] != 0x00) return;  // only the 0x14-byte input report
  PadState s = {};
  s.connected = true;
  s.up = d[2] & 0x01;
  s.down = d[2] & 0x02;
  s.left = d[2] & 0x04;
  s.right = d[2] & 0x08;
  s.start = d[2] & 0x10;
  s.select = d[2] & 0x20;
  s.a = d[3] & 0x10;
  s.b = d[3] & 0x20;
  s.x = d[3] & 0x40;
  s.y = d[3] & 0x80;
  int16_t lx = (int16_t)(d[6] | (d[7] << 8));
  s.lx = (int8_t)(lx / 258);
  setState(s);
}

static void xinputTransferCb(usb_transfer_t *t) {
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) xinputParse(t->data_buffer, t->actual_num_bytes);
  if (!xGone && t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED)
    usb_host_transfer_submit(t);
}

// Look for an XInput interface on a newly attached device and start reading it.
static void xinputTryOpen(uint8_t addr) {
  if (xDev) return;
  usb_device_handle_t dev;
  if (usb_host_device_open(xClient, addr, &dev) != ESP_OK) return;
  const usb_config_desc_t *cfg;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) { usb_host_device_close(xClient, dev); return; }

  int offset = 0;
  const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
  uint8_t intfNum = 0xFF, epAddr = 0;
  uint16_t epSize = 32;
  bool inXInput = false;
  while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != nullptr) {
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *in = (const usb_intf_desc_t *)d;
      inXInput = in->bInterfaceClass == 0xFF && in->bInterfaceSubClass == 0x5D && in->bInterfaceProtocol == 0x01;
      if (inXInput && intfNum == 0xFF) intfNum = in->bInterfaceNumber;
      else if (in->bInterfaceNumber != intfNum) inXInput = false;
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && inXInput && !epAddr) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
      if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 0x03) == 0x03) {
        epAddr = ep->bEndpointAddress;
        epSize = ep->wMaxPacketSize & 0x7FF;
      }
    }
  }
  if (intfNum == 0xFF || !epAddr || usb_host_interface_claim(xClient, dev, intfNum, 0) != ESP_OK) {
    usb_host_device_close(xClient, dev);
    return;
  }
  if (usb_host_transfer_alloc(epSize, 0, &xXfer) != ESP_OK) {
    usb_host_interface_release(xClient, dev, intfNum);
    usb_host_device_close(xClient, dev);
    return;
  }
  // Vendor "hello" request that Linux xpad sends to third-party 360-style pads before they
  // stream reports (bmRequestType 0xC1, bRequest 0x01, wValue 0x0100, 20 bytes). Fire-and-forget.
  usb_transfer_t *ctrl;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 20, 0, &ctrl) == ESP_OK) {
    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl->data_buffer;
    setup->bmRequestType = 0xC1;
    setup->bRequest = 0x01;
    setup->wValue = 0x0100;
    setup->wIndex = 0;
    setup->wLength = 20;
    ctrl->num_bytes = sizeof(usb_setup_packet_t) + 20;
    ctrl->device_handle = dev;
    ctrl->bEndpointAddress = 0;
    ctrl->callback = [](usb_transfer_t *t) { usb_host_transfer_free(t); };
    if (usb_host_transfer_submit_control(xClient, ctrl) != ESP_OK) usb_host_transfer_free(ctrl);
  }

  xDev = dev;
  xIntf = intfNum;
  xAddr = addr;
  xGone = false;
  xXfer->device_handle = dev;
  xXfer->bEndpointAddress = epAddr;
  xXfer->callback = xinputTransferCb;
  xXfer->num_bytes = epSize;
  usb_host_transfer_submit(xXfer);
  Serial.printf("xinput: addr %u intf %u ep 0x%02x\n", addr, intfNum, epAddr);
}

static void xinputClose() {
  if (!xDev) return;
  xGone = true;
  usb_host_endpoint_halt(xDev, xXfer->bEndpointAddress);
  usb_host_endpoint_flush(xDev, xXfer->bEndpointAddress);
  delay(5);
  usb_host_transfer_free(xXfer);
  usb_host_interface_release(xClient, xDev, xIntf);
  usb_host_device_close(xClient, xDev);
  xXfer = nullptr;
  xDev = nullptr;
}

static void xClientEvent(const usb_host_client_event_msg_t *msg, void *) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) xinputTryOpen(msg->new_dev.address);
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && xDev && msg->dev_gone.dev_hdl == xDev) {
    xinputClose();
    setState(PadState{});
  }
}

static void xClientTask(void *) {
  // pick up a controller that was already plugged in before this client registered
  uint8_t addrs[8];
  int n = 0;
  if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK)
    for (int i = 0; i < n; i++) xinputTryOpen(addrs[i]);
  for (;;) usb_host_client_handle_events(xClient, portMAX_DELAY);
}

void padBegin() {
  usb.onGamepad(onHidGamepad);
  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &d) {
    Serial.printf("usb: connected %04x:%04x addr %u\n", d.vid, d.pid, d.address);
  });
  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &d) {
    if (d.address == hidAddress) { hidAddress = -1; setState(PadState{}); }
  });
  if (!usb.begin()) {
    Serial.printf("usb.begin() failed: %s\n", usb.lastErrorName());
    return;
  }
  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = xClientEvent;
  cfg.async.callback_arg = nullptr;
  if (usb_host_client_register(&cfg, &xClient) != ESP_OK) return;
  xTaskCreatePinnedToCore(xClientTask, "xinput", 4096, nullptr, 5, nullptr, 0);
}
