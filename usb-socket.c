#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <poll.h>
#include <signal.h>

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

    int       indata;
    uint16_t  set_addr;

    QTAILQ_ENTRY(USBSocketDevice) next;
} USBSocketDevice;

static QTAILQ_HEAD(, USBSocketDevice) socketdevs = QTAILQ_HEAD_INITIALIZER(socketdevs);


static void socket_read(void *opaque)
{
    USBSocketDevice *s = opaque;
    uint8_t tag;
    socklen_t len=sizeof(struct sockaddr_un);
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
    printf("usb_socket: start recv %u\n",len);

    struct pollfd p={.fd=fd,.events=POLLIN};
    struct timespec t={.tv_sec=1,.tv_nsec=0};
    sigset_t all;
    sigfillset(&all);

    int ret=ppoll(&p,1,&t,&all);
    if(ret==0) {
        perror("usb_socket: timeout\n");
        return -1;
    } else if(ret!=1) {
        perror("usb_socket: poll failed\n");
        return -1;
    }

    printf("usb_socket: going to recv %u\n",len);

    uint8_t pre;
    struct iovec io[2]={{&pre,1},{data,len}};
    struct msghdr h={0,0,io,2,0,0,0};

    int rcvd=recvmsg(fd,&h,0);
    printf("usb_socket: recv %u (wanted %u)\n",rcvd,len);
    if(rcvd==1) {
       if(pre==1) ret=USB_RET_NAK;
       else if(pre==2) ret=USB_RET_STALL;
       else ret=0;
    } else if(rcvd<0) { ret=USB_RET_NODEV; }
    else ret=rcvd-1;

    printf("usb_socket: recv returns %u\n",ret);
    return ret;
}

static void dumphex(const char *s,const uint8_t *buf, int len) {
	int i;
	printf("%s",s);
	for(i=0;i<len;i++) { printf(" %02x",buf[i]); }
	printf("\n");
}

static int do_token_setup(USBDevice *dev, USBPacket *p) {
    USBSocketDevice *s = (USBSocketDevice *)dev;

    printf("usb_socket: setup %u\n",p->len);
    dumphex("  SETUP:",p->data,p->len);

    if(p->len>0xff) {
        printf("usb-socket: wtf, packet too big\n");
        return -1;
    }

    qemu_set_fd_handler(s->fd, NULL, NULL, s);

    int ret=-1;
    s->set_addr=(p->data[0]==0 && p->data[1]==0x05) ? (p->data[2]|(p->data[3]<<8)) : 0;
    s->indata=0;

    uint8_t pre[1]={0};
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
    USBSocketDevice *s = (USBSocketDevice *)dev;
    printf("usb_socket: in ep%u\n",p->devep);
    uint8_t pkt[2]={1,p->devep};

    if(p->devep==0) s->indata=1;
    
    if(send(s->fd,pkt,2,0)==2) {
        int ret=recv_reply(s->fd,p->data,p->len);
	if(s->set_addr) {
        	printf("usb-socket: set address to %04hx\n",s->set_addr);
		dev->addr=s->set_addr;
		s->set_addr=0;
	}
	return ret;
    }
    return -1;
}
static int do_token_out(USBDevice *dev, USBPacket *p) {
    USBSocketDevice *s = (USBSocketDevice *)dev;
    printf("usb_socket: out ep%u\n",p->devep);
    dumphex("  OUT:",p->data,p->len);

    if(p->devep==0 && p->len==0 && s->indata) return 0;

    int ret=-1;

    uint8_t pre[2]={2,p->devep};
    struct iovec io[2]={{pre,2},{p->data,p->len}};
    struct msghdr h={0,0,io,2,0,0,0};
    if(sendmsg(s->fd,&h,0)==1+p->len) {
        ret=recv_reply(s->fd,p->data,p->len);
    } else {
        perror("usb_socket: out failed");
    }

    return ret;
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

