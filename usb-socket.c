#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <poll.h>

#include "qemu-common.h"
#include "qemu-timer.h"
#include "monitor.h"
#include "sysemu.h"

#include "hw/usb.h"

typedef struct USBSocketDevice {
    USBDevice dev;
    int       fd;
    Notifier  exit;
    char      *path;
    struct    sockaddr_un remote;

    QTAILQ_ENTRY(USBSocketDevice) next;
} USBSocketDevice;

static QTAILQ_HEAD(, USBSocketDevice) socketdevs = QTAILQ_HEAD_INITIALIZER(socketdevs);


static void socket_read(void *opaque)
{
    USBSocketDevice *s = opaque;
    uint8_t tag;
    socklen_t len=sizeof(struct sockaddr_un);
    printf("usb-socket: len %u\n",len);
    if(recvfrom(s->fd,&tag,1,0,(struct sockaddr*)&s->remote,&len)==1) {
      if(tag==0xff) {
        printf("usb-socket: attach\n");
        if(connect(s->fd,(struct sockaddr*)&(s->remote),len)==0) {
            usb_device_attach(&s->dev);
        } else {
            perror("usb-socket: connect failed");
        }
      }
      if(tag==0xfe) {
        printf("usb-socket: detach\n");
        usb_device_detach(&s->dev);
      }
    }
}


static void usb_socket_exit_notifier(struct Notifier* n)
{
    //USBSocketDevice *s = container_of(n, USBSocketDevice, exit);
    // well... unplug here
}

static void usb_socket_handle_destroy(USBDevice *dev)
{
    USBSocketDevice *s = (USBSocketDevice *)dev;

    // well... detach?

    qemu_set_fd_handler(s->fd, NULL, NULL, NULL);
    if(s->path) free(s->path);
    QTAILQ_REMOVE(&socketdevs, s, next);
    qemu_remove_exit_notifier(&s->exit);
}


static int recv_reply(int fd, uint8_t *data, int len) {
    uint8_t pre;
    struct iovec io[2]={{&pre,1},{data,len}};
    struct msghdr h={0,0,io,2,0,0,0};
    int rcvd=recvmsg(fd,&h,0);
    if(rcvd==1) {
       if(pre==1) return USB_RET_NAK;
       if(pre==2) return USB_RET_STALL;
    }
    if(rcvd<0) return USB_RET_NODEV;
    return rcvd-1;
}

static int do_token_setup(USBDevice *dev, USBPacket *p) {
    USBSocketDevice *s = (USBSocketDevice *)dev;

    printf("usb_socket: setup %u\n",p->len);

    uint8_t pre[1]={0};
    if(p->len>0xff) {
        printf("usb-socket: wtf, packet too big\n");
        return -1;
    }

    qemu_set_fd_handler(s->fd, NULL, NULL, s);

    int ret=-1;

    struct iovec io[2]={{pre,1},{p->data,p->len}};
    struct msghdr h={0,0,io,2,0,0,0};
    if(sendmsg(s->fd,&h,0)==1+p->len) {
        ret=recv_reply(s->fd,p->data,p->len);
    } else {
        perror("usb_socket: setup failed");
    }
    qemu_set_fd_handler(s->fd, socket_read, NULL, s);
    return ret;
}

static int do_token_in(USBDevice *dev, USBPacket *p) {
    printf("usb_socket: in\n");
    return -1;
}
static int do_token_out(USBDevice *dev, USBPacket *p) {
    printf("usb_socket: out\n");
    return -1;
}

static int usb_socket_handle_packet(USBDevice *s, USBPacket *p)
{
    printf("usb_socket_handle_packet: pid=%u addr=%04x\n",p->pid,p->devaddr);

    switch(p->pid) {
    case USB_MSG_ATTACH:
        s->state = USB_STATE_ATTACHED;
        return 0;

    case USB_MSG_DETACH:
        s->state = USB_STATE_NOTATTACHED;
        return 0;

    case USB_MSG_RESET:
        s->remote_wakeup = 0;
        s->addr = 0;
        s->state = USB_STATE_DEFAULT;
        return 0;
    }

    if (s->state < USB_STATE_DEFAULT || p->devaddr != s->addr)
        return USB_RET_NODEV;

    switch (p->pid) {
    case USB_TOKEN_SETUP:
        return do_token_setup(s, p);

    case USB_TOKEN_IN:
        return do_token_in(s, p);

    case USB_TOKEN_OUT:
        return do_token_out(s, p);
 
    default:
        return USB_RET_STALL;
    }
}


static int usb_socket_initfn(USBDevice *dev)
{
    USBSocketDevice *s = DO_UPCAST(USBSocketDevice, dev, dev);

    dev->auto_attach = 0;
    dev->speed = USB_SPEED_FULL;
    s->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un remote={.sun_family=AF_UNIX};
    strncpy(remote.sun_path,s->path,sizeof(remote.sun_path)-1);

    unlink(remote.sun_path);
    if( bind(s->fd, (struct sockaddr *)&remote, strlen(remote.sun_path) + sizeof(remote.sun_family)) == -1) {
	close(s->fd);
	s->fd=-1;
        perror("usb socket bind");
	return -1;
    }

    printf("usb-socket: initfn\n");

    qemu_set_fd_handler(s->fd, socket_read, NULL, s);

    QTAILQ_INSERT_TAIL(&socketdevs, s, next);
    s->exit.notify = usb_socket_exit_notifier;
    qemu_add_exit_notifier(&s->exit);
    return 0;
}

USBDevice *usb_socket_device_open(const char *devname)
{
    USBDevice *dev;

    dev = usb_create(NULL /* FIXME */, "usb-socket");
    USBSocketDevice *s = DO_UPCAST(USBSocketDevice, dev, dev);

    s->path=strdup(devname);
    qdev_prop_set_string(&dev->qdev, "usbsockpath", s->path);
    qdev_init_nofail(&dev->qdev);
    return dev;

//fail:
    //qdev_free(&dev->qdev);
    //return NULL;
}


static struct USBDeviceInfo usb_socket_dev_info = {
    .product_desc   = "USB Socket Device",
    .qdev.name      = "usb-socket",
    .qdev.size      = sizeof(USBSocketDevice),
    .init           = usb_socket_initfn,


    .handle_packet  = usb_socket_handle_packet,

    .handle_destroy = usb_socket_handle_destroy,
    .usbdevice_name = "socket",
    .usbdevice_init = usb_socket_device_open,


    .qdev.props     = (Property[]) {
        DEFINE_PROP_STRING("usbsockpath", USBSocketDevice, path),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void usb_socket_register_devices(void)
{
    usb_qdev_register(&usb_socket_dev_info);
}
device_init(usb_socket_register_devices)

int usb_socket_device_close(const char *devname)
{
    return -1;
}

