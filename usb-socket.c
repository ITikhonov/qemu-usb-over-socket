#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>

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

    QTAILQ_ENTRY(USBSocketDevice) next;
} USBSocketDevice;

static QTAILQ_HEAD(, USBSocketDevice) socketdevs = QTAILQ_HEAD_INITIALIZER(socketdevs);

static void usb_socket_exit_notifier(struct Notifier* n)
{
    //USBSocketDevice *s = container_of(n, USBSocketDevice, exit);
    // well... unplug here
}

static void usb_socket_handle_reset(USBDevice *dev)
{
    // USBSocketDevice *s = DO_UPCAST(USBSocketDevice, dev, dev);
    // well... reset here
}

static void usb_socket_handle_destroy(USBDevice *dev)
{
    USBSocketDevice *s = (USBSocketDevice *)dev;

    // well... detach?

    if(s->path) free(s->path);
    QTAILQ_REMOVE(&socketdevs, s, next);
    qemu_remove_exit_notifier(&s->exit);
}


static int send_to_sock(int id, USBDevice *dev, USBPacket *p) {
    USBSocketDevice *s = (USBSocketDevice *)dev;
    uint8_t pre[2]={0,p->len};
    if(p->len>0xff) {
        printf("usb-socket: wtf, packet too big\n");
        return -1;
    }
    struct iovec io[2]={{pre,2},{p->data,p->len}};
    struct msghdr h={0,0,io,2,0,0,0};
    return !(sendmsg(s->fd,&h,0)==2+p->len);
}

static int usb_socket_handle_control(USBDevice *dev, int request, int value,
                                    int index, int length, uint8_t *data)
{
    USBSocketDevice *s = (USBSocketDevice *)dev;
    uint8_t pkt[10]={0,8,request>>8,request,value,value>>8,index,index>>8,length,length>>8};
    if(send(s->fd,pkt,10,0)!=10) return -1;
    return recv(s->fd,data,length,0);
}

static int usb_socket_handle_data(USBDevice *dev, USBPacket *p) {
    USBSocketDevice *s = (USBSocketDevice *)dev;
    switch (p->pid) {
    case USB_TOKEN_IN:
        if(send_to_sock(1,dev,p)) return -1;
        return recv(s->fd,p->data,p->len,0);
    case USB_TOKEN_OUT:
        if(send_to_sock(2,dev,p)) return -1;
        return 0;
    default:
        return USB_RET_STALL;
    }

}

static int usb_socket_initfn(USBDevice *dev)
{
    USBSocketDevice *s = DO_UPCAST(USBSocketDevice, dev, dev);

    dev->speed = USB_SPEED_FULL;
    s->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un remote={.sun_family=AF_UNIX};
    strncpy(remote.sun_path,s->path,sizeof(remote.sun_path)-1);
    if (connect(s->fd, (struct sockaddr *)&remote, strlen(remote.sun_path) + sizeof(remote.sun_family)) == -1) {
	close(s->fd);
	s->fd=-1;
        perror("usb socket connect");
	return -1;
    }

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


    .handle_packet  = usb_generic_handle_packet,
    .handle_control = usb_socket_handle_control,
    .handle_data    = usb_socket_handle_data,

    .handle_reset   = usb_socket_handle_reset,
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

