/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal atomic DRM backend. Own implementation; no SurfaceFlinger dependency. */
#ifndef QWEOVO_BOOTMENU_DRM_H
#define QWEOVO_BOOTMENU_DRM_H
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#define BM_MAX_OBJECTS 16
#define BM_MAX_PROPS 192
#define BM_MAX_IDS 64
#define BM_DRM_NODE "/dev/.qweovo-bootmenu-card0"

struct bm_atomic {
    __u32 ids[BM_MAX_OBJECTS], counts[BM_MAX_OBJECTS], props[BM_MAX_PROPS];
    __u64 values[BM_MAX_PROPS], old_values[BM_MAX_PROPS];
    unsigned objects, properties;
};
struct bm_buffer { __u32 handle, fb, pitch; __u64 size; unsigned *pixels; };
struct bm_display {
    int fd, width, height, crtc_index, plane_count, current, shown;
    __u32 connector, crtc, planes[4], mode_blob;
    struct drm_mode_modeinfo mode;
    struct bm_atomic saved;
    struct bm_buffer buffers[2];
};

static int bm_ioctl(int fd, unsigned long command, void *arg)
{
    int ret;
    do { ret = syscall(__NR_ioctl, fd, command, arg); } while (ret < 0 && errno == EINTR);
    return ret;
}

static int bm_read_text(const char *path, char *buffer, int size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC), n;
    if (fd < 0) return -1;
    n = read(fd, buffer, size-1);
    close(fd);
    if (n < 0) return -1;
    buffer[n] = 0;
    return n;
}

/* First-stage init may not have created non-block device nodes yet. */
static void bm_ensure_node(const char *sysfs, const char *path)
{
    char buf[64], *colon;
    unsigned major, minor;
    struct stat st;
    if (!lstat(path, &st)) return;
    if (bm_read_text(sysfs, buf, sizeof(buf)) < 0 || !(colon = strchr(buf, ':'))) return;
    *colon++ = 0;
    major = atoi(buf); minor = atoi(colon);
    if (major > 4095 || minor > 255) return;
    mknod(path, S_IFCHR | 0600, (major << 8) | minor);
}

static int bm_property(int fd, __u32 object, __u32 type, const char *name,
                       __u32 *property, __u64 *value)
{
    struct drm_mode_obj_get_properties req = { .obj_id=object, .obj_type=type };
    __u32 ids[BM_MAX_PROPS];
    __u64 vals[BM_MAX_PROPS];
    unsigned i;
    if (bm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req) || req.count_props > BM_MAX_PROPS) return -1;
    req.props_ptr = (__u64)(unsigned long)ids;
    req.prop_values_ptr = (__u64)(unsigned long)vals;
    req.count_props = BM_MAX_PROPS;
    if (bm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req) || req.count_props > BM_MAX_PROPS) return -1;
    for (i=0; i<req.count_props; i++) {
        struct drm_mode_get_property p = {.prop_id=ids[i]};
        if (bm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) continue;
        if (!strcmp(p.name, name)) {
            if (property) *property=ids[i];
            if (value) *value=vals[i];
            return 0;
        }
    }
    return -1;
}

static int bm_add(struct bm_display *d, struct bm_atomic *a, __u32 object,
                   __u32 type, const char *name, __u64 value)
{
    __u32 prop;
    __u64 old;
    if (a->properties >= BM_MAX_PROPS || bm_property(d->fd, object, type, name, &prop, &old)) return -1;
    if (!a->objects || a->ids[a->objects-1] != object) {
        if (a->objects >= BM_MAX_OBJECTS) return -1;
        a->ids[a->objects]=object;
        a->counts[a->objects++]=0;
    }
    a->counts[a->objects-1]++;
    a->props[a->properties]=prop;
    a->values[a->properties]=value;
    a->old_values[a->properties++]=old;
    return 0;
}

static int bm_atomic_commit(struct bm_display *d, struct bm_atomic *a, unsigned flags)
{
    struct drm_mode_atomic req = {
        .flags=flags, .count_objs=a->objects,
        .objs_ptr=(__u64)(unsigned long)a->ids,
        .count_props_ptr=(__u64)(unsigned long)a->counts,
        .props_ptr=(__u64)(unsigned long)a->props,
        .prop_values_ptr=(__u64)(unsigned long)a->values
    };
    return bm_ioctl(d->fd, DRM_IOCTL_MODE_ATOMIC, &req);
}

static int bm_topology(struct bm_display *d)
{
    struct drm_mode_get_blob blob = {0};
    __u64 id;
    char text[8193];
    if (bm_property(d->fd, d->connector, DRM_MODE_OBJECT_CONNECTOR, "mode_properties", NULL, &id)) return 1;
    blob.blob_id=id;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) || blob.length > sizeof(text)-1) return 1;
    blob.data=(__u64)(unsigned long)text;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) || blob.length > sizeof(text)-1) return 1;
    text[blob.length]=0;
    if (strstr(text, "topology=sde_quad")) return 4;
    if (strstr(text, "topology=sde_dual")) return 2;
    return 1;
}

static int bm_find_panel(struct bm_display *d)
{
    struct drm_mode_card_res res = {0};
    __u32 connectors[BM_MAX_IDS], crtcs[BM_MAX_IDS];
    unsigned i, j;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) ||
        !res.count_crtcs || res.count_crtcs > BM_MAX_IDS || res.count_connectors > BM_MAX_IDS) return -1;
    res.connector_id_ptr=(__u64)(unsigned long)connectors;
    res.crtc_id_ptr=(__u64)(unsigned long)crtcs;
    res.count_crtcs=BM_MAX_IDS; res.count_connectors=BM_MAX_IDS;
    res.count_fbs=0; res.count_encoders=0;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) ||
        res.count_crtcs > BM_MAX_IDS || res.count_connectors > BM_MAX_IDS) return -1;
    for (i=0; i<res.count_connectors; i++) {
        struct drm_mode_get_connector conn = {.connector_id=connectors[i]};
        struct drm_mode_modeinfo modes[BM_MAX_IDS];
        __u32 encoders[BM_MAX_IDS];
        int chosen=0;
        if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) ||
            conn.connector_type != DRM_MODE_CONNECTOR_DSI || conn.connection != 1 ||
            !conn.count_modes || conn.count_modes > BM_MAX_IDS || conn.count_encoders > BM_MAX_IDS) continue;
        conn.modes_ptr=(__u64)(unsigned long)modes;
        conn.encoders_ptr=(__u64)(unsigned long)encoders;
        conn.count_modes=BM_MAX_IDS; conn.count_encoders=BM_MAX_IDS; conn.count_props=0;
        if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) ||
            !conn.count_modes || conn.count_modes > BM_MAX_IDS || conn.count_encoders > BM_MAX_IDS) continue;
        /* Preserve the panel's preferred boot timing. Forcing 60 Hz during
         * continuous splash is rejected by this DSI driver's first frame. */
        for (j=0; j<conn.count_modes; j++)
            if (modes[j].type & DRM_MODE_TYPE_PREFERRED) { chosen=j; break; }
        d->mode=modes[chosen];
        if (d->mode.hdisplay < 320 || d->mode.vdisplay < 320 ||
            d->mode.hdisplay > 8192 || d->mode.vdisplay > 8192) continue;
        for (j=0; j<conn.count_encoders; j++) {
            struct drm_mode_get_encoder enc = {.encoder_id=encoders[j]};
            unsigned c;
            if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &enc)) continue;
            for (c=0; c<res.count_crtcs && c<32; c++) {
                if (!(enc.possible_crtcs & (1U<<c))) continue;
                if (enc.crtc_id && enc.crtc_id != crtcs[c]) continue;
                d->crtc=crtcs[c]; d->crtc_index=c;
                d->connector=conn.connector_id;
                d->width=d->mode.hdisplay; d->height=d->mode.vdisplay;
                klog("bootmenu: panel %dx%d refresh=%u preferred=%u",d->width,d->height,
                     d->mode.vrefresh,!!(d->mode.type&DRM_MODE_TYPE_PREFERRED));
                return 0;
            }
        }
    }
    return -1;
}

static int bm_find_planes(struct bm_display *d, __u32 *planes)
{
    struct drm_mode_get_plane_res req = {0};
    __u32 ids[BM_MAX_IDS];
    int count=0;
    unsigned i, j;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &req) || req.count_planes > BM_MAX_IDS) return 0;
    req.plane_id_ptr=(__u64)(unsigned long)ids;
    req.count_planes=BM_MAX_IDS;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &req) || req.count_planes > BM_MAX_IDS) return 0;
    for (i=0; i<req.count_planes && count<4; i++) {
        struct drm_mode_get_plane plane = {.plane_id=ids[i]};
        __u32 formats[BM_MAX_IDS];
        __u64 type=0;
        if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETPLANE, &plane) ||
            !(plane.possible_crtcs & (1U<<d->crtc_index)) || plane.count_format_types > BM_MAX_IDS) continue;
        if ((plane.crtc_id && plane.crtc_id!=d->crtc) ||
            (!bm_property(d->fd,plane.plane_id,DRM_MODE_OBJECT_PLANE,"type",NULL,&type) && type==2)) continue;
        plane.format_type_ptr=(__u64)(unsigned long)formats;
        plane.count_format_types=BM_MAX_IDS;
        if (bm_ioctl(d->fd, DRM_IOCTL_MODE_GETPLANE, &plane) || plane.count_format_types > BM_MAX_IDS) continue;
        for (j=0; j<plane.count_format_types; j++) if (formats[j] == DRM_FORMAT_XBGR8888) {
            planes[count++]=plane.plane_id;
            break;
        }
    }
    return count;
}

static int bm_buffer_create(struct bm_display *d, struct bm_buffer *b)
{
    struct drm_mode_create_dumb dumb = {.width=d->width, .height=d->height, .bpp=32};
    struct drm_mode_fb_cmd2 fb = {.width=d->width, .height=d->height, .pixel_format=DRM_FORMAT_XBGR8888};
    struct drm_mode_map_dumb map = {0};
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) return -1;
    b->handle=dumb.handle; b->pitch=dumb.pitch; b->size=dumb.size;
    if (b->pitch < (unsigned)d->width*4 || b->size < (__u64)b->pitch*d->height || b->size > 256U*1024*1024) return -1;
    fb.handles[0]=dumb.handle; fb.pitches[0]=dumb.pitch;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &fb)) return -1;
    b->fb=fb.fb_id; map.handle=dumb.handle;
    if (bm_ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) return -1;
    b->pixels=mmap(NULL, b->size, PROT_READ|PROT_WRITE, MAP_SHARED, d->fd, map.offset);
    if (b->pixels == MAP_FAILED) { b->pixels=NULL; return -1; }
    memset(b->pixels, 0, b->size);
    return 0;
}

static int bm_plane_props(struct bm_display *d, struct bm_atomic *a, __u32 plane,
                           __u32 fb, int x, int width)
{
    return bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"FB_ID",fb) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"CRTC_ID",d->crtc) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"SRC_X",(__u64)x<<16) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"SRC_Y",0) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"SRC_W",(__u64)width<<16) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"SRC_H",(__u64)d->height<<16) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"CRTC_X",x) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"CRTC_Y",0) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"CRTC_W",width) ||
        bm_add(d,a,plane,DRM_MODE_OBJECT_PLANE,"CRTC_H",d->height);
}

static int bm_modeset(struct bm_display *d, __u32 *planes, int count)
{
    struct bm_atomic a = {0};
    static unsigned reported;
    int i;
    if (d->width % count) return -1;
    if (bm_add(d,&a,d->connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",d->crtc) ||
        bm_add(d,&a,d->crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",d->mode_blob) ||
        bm_add(d,&a,d->crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",1)) return -1;
    for (i=0; i<count; i++)
        if (bm_plane_props(d,&a,planes[i],d->buffers[0].fb,d->width*i/count,d->width/count)) return -1;
    if (bm_atomic_commit(d,&a,DRM_MODE_ATOMIC_TEST_ONLY|DRM_MODE_ATOMIC_ALLOW_MODESET)) {
        if (reported++<3) klog("bootmenu: atomic test planes=%d failed: %d",count,errno);
        return -1;
    }
    if (bm_atomic_commit(d,&a,DRM_MODE_ATOMIC_ALLOW_MODESET)) {
        if (reported++<3) klog("bootmenu: atomic commit planes=%d failed: %d",count,errno);
        return -1;
    }
    d->saved=a;
    memcpy(d->saved.values,a.old_values,sizeof(a.old_values));
    memcpy(d->planes,planes,count*sizeof(*planes));
    d->plane_count=count; d->shown=1; d->current=1;
    return 0;
}

static void bm_display_close(struct bm_display *d)
{
    int i;
    if (d->fd < 0) return;
    if (d->shown && bm_atomic_commit(d,&d->saved,DRM_MODE_ATOMIC_ALLOW_MODESET)) {
        struct bm_atomic disable={0};
        int failed=0;
        klog("bootmenu: display restore failed: %d",errno);
        /* A previous DRM client's FB/mode blob may have disappeared. Release
         * our scanout cleanly instead of restoring stale object IDs. Android
         * will perform its own modeset after the early menu exits. */
        failed|=bm_add(d,&disable,d->connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",0);
        failed|=bm_add(d,&disable,d->crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",0);
        failed|=bm_add(d,&disable,d->crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",0);
        for (i=0;i<d->plane_count;i++) {
            failed|=bm_add(d,&disable,d->planes[i],DRM_MODE_OBJECT_PLANE,"FB_ID",0);
            failed|=bm_add(d,&disable,d->planes[i],DRM_MODE_OBJECT_PLANE,"CRTC_ID",0);
        }
        if (failed || bm_atomic_commit(d,&disable,DRM_MODE_ATOMIC_ALLOW_MODESET))
            klog("bootmenu: scanout disable failed: %d",errno);
    }
    for (i=0; i<2; i++) {
        struct bm_buffer *b=&d->buffers[i];
        struct drm_mode_destroy_dumb destroy = {.handle=b->handle};
        if (b->pixels) munmap(b->pixels,b->size);
        if (b->fb) bm_ioctl(d->fd,DRM_IOCTL_MODE_RMFB,&b->fb);
        if (b->handle) bm_ioctl(d->fd,DRM_IOCTL_MODE_DESTROY_DUMB,&destroy);
    }
    if (d->mode_blob) {
        struct drm_mode_destroy_blob blob={.blob_id=d->mode_blob};
        bm_ioctl(d->fd,DRM_IOCTL_MODE_DESTROYPROPBLOB,&blob);
    }
    bm_ioctl(d->fd,DRM_IOCTL_DROP_MASTER,NULL);
    close(d->fd); d->fd=-1;
}

static int bm_display_open(struct bm_display *d)
{
    struct drm_set_client_cap cap={.capability=DRM_CLIENT_CAP_UNIVERSAL_PLANES,.value=1};
    struct drm_mode_create_blob blob={0};
    __u32 planes[4];
    int available, preferred, i;
    const char *step="master";
    memset(d,0,sizeof(*d)); d->fd=-1;
    /* Never create Android's device directories before SELinux loads. A
     * pre-policy /dev/dri inherits tmpfs:s0 and prevents ueventd from creating
     * the correctly labelled graphics nodes during coldboot. */
    bm_ensure_node("/sys/class/drm/card0/dev",BM_DRM_NODE);
    d->fd=open(BM_DRM_NODE,O_RDWR|O_CLOEXEC);
    if (d->fd < 0) {
        static int reported;
        if (!reported++) klog("bootmenu: DRM node unavailable: %d",errno);
        return -1;
    }
    if (bm_ioctl(d->fd,DRM_IOCTL_SET_MASTER,NULL)) goto fail;
    step="universal planes";
    if (bm_ioctl(d->fd,DRM_IOCTL_SET_CLIENT_CAP,&cap)) goto fail;
    cap.capability=DRM_CLIENT_CAP_ATOMIC;
    step="atomic capability";
    if (bm_ioctl(d->fd,DRM_IOCTL_SET_CLIENT_CAP,&cap)) goto fail;
    step="panel resources";
    if (bm_find_panel(d)) goto fail;
    step="planes";
    available=bm_find_planes(d,planes);
    if (!available) goto fail;
    step="buffer 0";
    if (bm_buffer_create(d,&d->buffers[0])) goto fail;
    step="buffer 1";
    if (bm_buffer_create(d,&d->buffers[1])) goto fail;
    blob.data=(__u64)(unsigned long)&d->mode; blob.length=sizeof(d->mode);
    step="mode blob";
    if (bm_ioctl(d->fd,DRM_IOCTL_MODE_CREATEPROPBLOB,&blob)) goto fail;
    d->mode_blob=blob.blob_id;
    preferred=bm_topology(d);
    step="modeset";
    if (preferred<=available && !bm_modeset(d,planes,preferred)) return 0;
    for (i=1; i<=4; i*=2)
        if (i!=preferred && i<=available && !bm_modeset(d,planes,i)) return 0;
fail:
    {
        static int reported;
        if (!reported++) klog("bootmenu: DRM setup failed at %s: %d",step,errno);
    }
    bm_display_close(d);
    return -1;
}

static int bm_display_present(struct bm_display *d)
{
    struct bm_atomic a={0};
    int i;
    for (i=0; i<d->plane_count; i++)
        if (bm_add(d,&a,d->planes[i],DRM_MODE_OBJECT_PLANE,"FB_ID",d->buffers[d->current].fb)) return -1;
    if (bm_atomic_commit(d,&a,DRM_MODE_ATOMIC_ALLOW_MODESET)) return -1;
    d->current^=1;
    return 0;
}
#endif
