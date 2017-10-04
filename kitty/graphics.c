/*
 * graphics.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "graphics.h"
#include "state.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>

#include <zlib.h>
#include <png.h>

#define REPORT_ERROR(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }

static bool send_to_gpu = true;

GraphicsManager*
grman_realloc(GraphicsManager *old, index_type lines, index_type columns) {
    GraphicsManager *self = (GraphicsManager *)GraphicsManager_Type.tp_alloc(&GraphicsManager_Type, 0);
    self->lines = lines; self->columns = columns;
    if (old == NULL) {
        self->images_capacity = 64;
        self->images = calloc(self->images_capacity, sizeof(Image));
        if (self->images == NULL) {
            Py_CLEAR(self); return NULL;
        }
    } else {
        self->images_capacity = old->images_capacity; self->images = old->images; self->image_count = old->image_count;
        old->images = NULL;
        Py_DECREF(old);
    }
    return self;
}

static inline void
free_refs_data(Image *img) {
    free(img->refs); img->refs = NULL;
    img->refcnt = 0; img->refcap = 0;
}

static inline void
free_load_data(LoadData *ld) {
    free(ld->buf); ld->buf_used = 0; ld->buf_capacity = 0;
    ld->buf = NULL;

    if (ld->mapped_file) munmap(ld->mapped_file, ld->mapped_file_sz);
    ld->mapped_file = NULL; ld->mapped_file_sz = 0;
}

free_texture_func free_texture = NULL;
send_image_to_gpu_func send_image_to_gpu = NULL;

static inline void
free_image(Image *img) {
    if (img->texture_id) free_texture(&img->texture_id);
    free_refs_data(img);
    free_load_data(&(img->load_data));
}

static void
dealloc(GraphicsManager* self) {
    size_t i;
    if (self->images) {
        for (i = 0; i < self->image_count; i++) free_image(self->images + i);
        free(self->images);
    }
    free(self->render_data);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static size_t internal_id_counter = 1;

static inline void*
ensure_space(void *array, size_t *capacity, size_t count, size_t item_size, bool initialize) {
    if (count < *capacity) return array;
    void *ans = realloc(array, (*capacity) * item_size * 2);
    if (ans == NULL) fatal("Out of memory re-allocating array.");
    if (initialize) {
        memset(((uint8_t*)array) + ((*capacity) * item_size), 0, ((*capacity) * item_size));
    }
    *capacity *= 2;
    return ans;
}

static inline void
remove_from_array(void *array, size_t item_size, size_t idx, size_t array_count) {
    size_t num_to_right = array_count - 1 - idx;
    uint8_t *p = (uint8_t*)array;
    if (num_to_right > 0) memmove(p + (idx * item_size), p + ((idx + 1) * item_size), num_to_right * item_size);  
    memset(p + (item_size * (array_count - 1)), 0, item_size);
}

static inline Image*
img_by_internal_id(GraphicsManager *self, size_t id) {
    for (size_t i = 0; i < self->image_count; i++) {
        if (self->images[i].internal_id == id) return self->images + i;
    }
    return NULL;
}

static inline Image*
img_by_client_id(GraphicsManager *self, uint32_t id) {
    for (size_t i = 0; i < self->image_count; i++) {
        if (self->images[i].client_id == id) return self->images + i;
    }
    return NULL;
}


// Loading image data {{{

static char add_response[512] = {0};
static bool has_add_respose = false;

static inline void
set_add_response(const char *code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    size_t sz = sizeof(add_response)/sizeof(add_response[0]);
    int num = snprintf(add_response, sz, "%s:", code);
    vsnprintf(add_response + num, sz - num, fmt, args);
    va_end(args);
    has_add_respose = true;
}

// Decode formats {{{
#define ABRT(code, ...) { set_add_response(#code, __VA_ARGS__); goto err; }

static inline bool
mmap_img_file(GraphicsManager UNUSED *self, Image *img, int fd, size_t sz, off_t offset) {
    if (!sz) {
        struct stat s;
        if (fstat(fd, &s) != 0) ABRT(EBADF, "Failed to fstat() the fd: %d file with error: [%d] %s", fd, errno, strerror(errno));
        sz = s.st_size;
    }
    void *addr = mmap(0, sz, PROT_READ, MAP_SHARED, fd, offset);
    if (addr == MAP_FAILED) ABRT(EBADF, "Failed to map image file fd: %d at offset: %zd with size: %zu with error: [%d] %s", fd, offset, sz, errno, strerror(errno)); 
    img->load_data.mapped_file = addr;
    img->load_data.mapped_file_sz = sz;
    return true;
err:
    return false;
}


static inline const char*
zlib_strerror(int ret) {
#define Z(x) case x: return #x;
    static char buf[128];
    switch(ret) {
        case Z_ERRNO:
            return strerror(errno);
        default:
            snprintf(buf, sizeof(buf)/sizeof(buf[0]), "Unknown error: %d", ret);
            return buf;
        Z(Z_STREAM_ERROR);
        Z(Z_DATA_ERROR);
        Z(Z_MEM_ERROR);
        Z(Z_BUF_ERROR);
        Z(Z_VERSION_ERROR);
    }
#undef Z
}

static inline bool
inflate_zlib(GraphicsManager UNUSED *self, Image *img, uint8_t *buf, size_t bufsz) {
    bool ok = false;
    z_stream z;
    uint8_t *decompressed = malloc(img->load_data.data_sz);
    if (decompressed == NULL) fatal("Out of memory allocating decompression buffer");
    z.zalloc = Z_NULL;
    z.zfree = Z_NULL;
    z.opaque = Z_NULL;
    z.avail_in = bufsz;
    z.next_in = (Bytef*)buf;
    z.avail_out = img->load_data.data_sz;
    z.next_out = decompressed;
    int ret;
    if ((ret = inflateInit(&z)) != Z_OK) ABRT(ENOMEM, "Failed to initialize inflate with error: %s", zlib_strerror(ret));
    if ((ret = inflate(&z, Z_FINISH)) != Z_STREAM_END) ABRT(EINVAL, "Failed to inflate image data with error: %s", zlib_strerror(ret));
    if (z.avail_out) ABRT(EINVAL, "Image data size post inflation does not match expected size");
    free_load_data(&img->load_data);
    img->load_data.buf_capacity = img->load_data.data_sz;
    img->load_data.buf = decompressed;
    img->load_data.buf_used = img->load_data.data_sz;
    ok = true;
err:
    inflateEnd(&z);
    if (!ok) free(decompressed);
    return ok;
}

struct fake_file { uint8_t *buf; size_t sz, cur; };

static void
read_png_from_buffer(png_structp png, png_bytep out, png_size_t length) {
    struct fake_file *f = png_get_io_ptr(png);
    if (f) {
        size_t amt = MIN(length, f->sz - f->cur);
        memcpy(out, f->buf + f->cur, amt);
        f->cur += amt;
    }
}

static void
read_png_error_handler(png_structp png_ptr, png_const_charp msg) {
    jmp_buf *jb;
    set_add_response("EBADPNG", msg);
    jb = png_get_error_ptr(png_ptr);
    if (jb == NULL) fatal("read_png_error_handler: could not retrieve jmp_buf");
    longjmp(*jb, 1);
}

static void
read_png_warn_handler(png_structp UNUSED png_ptr, png_const_charp UNUSED msg) {
    // ignore warnings
}

struct png_jmp_data { uint8_t *decompressed; bool ok; png_bytep *row_pointers; int width, height; size_t sz; };

static void
inflate_png_inner(struct png_jmp_data *d, uint8_t *buf, size_t bufsz) {
    struct fake_file f = {.buf = buf, .sz = bufsz};
    png_structp png = NULL;
    png_infop info = NULL;
    jmp_buf jb;
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &jb, read_png_error_handler, read_png_warn_handler);
    if (!png) ABRT(ENOMEM, "Failed to create PNG read structure");
    info = png_create_info_struct(png);
    if (!info) ABRT(ENOMEM, "Failed to create PNG info structure");

    if (setjmp(jb)) goto err;
    
    png_set_read_fn(png, &f, read_png_from_buffer);
    png_read_info(png, info);
    png_byte color_type, bit_depth;
    d->width      = png_get_image_width(png, info);
    d->height     = png_get_image_height(png, info);
    color_type = png_get_color_type(png, info);
    bit_depth  = png_get_bit_depth(png, info);

    // Ensure we get RGBA data out of libpng
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);

    // These color_type don't have an alpha channel then fill it with 0xff.
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    int rowbytes = png_get_rowbytes(png, info);
    d->sz = rowbytes * d->height * sizeof(png_byte);
    d->decompressed = malloc(d->sz + 16);
    if (d->decompressed == NULL) ABRT(ENOMEM, "Out of memory allocating decompression buffer for PNG");
    d->row_pointers = malloc(d->height * sizeof(png_bytep));
    if (d->row_pointers == NULL) ABRT(ENOMEM, "Out of memory allocating row_pointers buffer for PNG");
    for (int i = 0; i < d->height; i++) d->row_pointers[d->height - 1 - i] = d->decompressed + i * rowbytes;
    png_read_image(png, d->row_pointers);

    d->ok = true;
err:
    if (png) png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    return;
}

static inline bool
inflate_png(GraphicsManager UNUSED *self, Image *img, uint8_t *buf, size_t bufsz) {
    struct png_jmp_data d = {0};
    inflate_png_inner(&d, buf, bufsz);
    if (d.ok) {
        free_load_data(&img->load_data);
        img->load_data.buf = d.decompressed;
        img->load_data.buf_capacity = d.sz; 
        img->load_data.buf_used = d.sz;
        img->load_data.data_sz = d.sz;
        img->width = d.width; img->height = d.height;
    }
    else free(d.decompressed);
    free(d.row_pointers);
    return d.ok;
}
#undef ABRT
// }}}

static bool
add_trim_predicate(Image *img) {
    return !img->data_loaded || (!img->client_id && !img->refcnt);
}

static inline Image*
find_or_create_image(GraphicsManager *self, uint32_t id, bool *existing) {
    if (id) {
        for (size_t i = 0; i < self->image_count; i++) {
            if (self->images[i].client_id == id) {
                *existing = true;
                return self->images + i;
            }
        }
    }
    *existing = false;
    self->images = ensure_space(self->images, &self->images_capacity, self->image_count, sizeof(Image), true);
    return self->images + self->image_count++;
}

static inline void
remove_images(GraphicsManager *self, bool(*predicate)(Image*)) {
    for (size_t i = self->image_count; i-- > 0;) {
        if (predicate(self->images + i)) {
            free_image(self->images + i);
            remove_from_array(self->images, sizeof(Image), i, self->image_count--);
            self->layers_dirty = true;
        }
    }
}


static Image*
handle_add_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, bool *is_dirty) {
#define ABRT(code, ...) { set_add_response(#code, __VA_ARGS__); self->loading_image = 0; if (img) img->data_loaded = false; return NULL; }
#define MAX_DATA_SZ (4 * 100000000)
    has_add_respose = false;
    bool existing, init_img = true;
    Image *img = NULL;
    unsigned char tt = g->transmission_type ? g->transmission_type : 'd';
    enum FORMATS { RGB=24, RGBA=32, PNG=100 };
    uint32_t fmt = g->format ? g->format : RGBA;
    if (tt == 'd' && self->loading_image) init_img = false;
    if (init_img) {
        self->last_init_graphics_command = *g;
        self->loading_image = 0;
        if (g->data_width > 10000 || g->data_height > 10000) ABRT(EINVAL, "Image too large");
        remove_images(self, add_trim_predicate);
        img = find_or_create_image(self, g->id, &existing);
        if (existing) {
            free_load_data(&img->load_data);
            img->data_loaded = false;
            free_refs_data(img);
            *is_dirty = true;
            self->layers_dirty = true;
        } else {
            img->internal_id = internal_id_counter++;
            img->client_id = g->id;
        }
        img->width = g->data_width; img->height = g->data_height;
        switch(fmt) {
            case PNG:  
                if (g->data_sz > MAX_DATA_SZ) ABRT(EINVAL, "PNG data size too large");
                img->load_data.is_4byte_aligned = true;
                img->load_data.is_opaque = false;
                img->load_data.data_sz = g->data_sz ? g->data_sz : 1024 * 100;
                break;
            case RGB:
            case RGBA:
                img->load_data.data_sz = g->data_width * g->data_height * (fmt / 8);
                if (!img->load_data.data_sz) ABRT(EINVAL, "Zero width/height not allowed");
                img->load_data.is_4byte_aligned = fmt == RGBA || (img->width % 4 == 0);
                img->load_data.is_opaque = fmt == RGB;
                break;
            default:
                ABRT(EINVAL, "Unknown image format: %u", fmt);
        }
        if (tt == 'd') {
            if (g->more) self->loading_image = img->internal_id;
            img->load_data.buf_capacity = img->load_data.data_sz + (g->compressed ? 1024 : 10);  // compression header
            img->load_data.buf = malloc(img->load_data.buf_capacity);
            img->load_data.buf_used = 0;
            if (img->load_data.buf == NULL) {
                ABRT(ENOMEM, "Out of memory");
                img->load_data.buf_capacity = 0; img->load_data.buf_used = 0;
            }
        }
    } else {
        self->last_init_graphics_command.more = g->more;
        self->last_init_graphics_command.payload_sz = g->payload_sz;
        g = &self->last_init_graphics_command;
        tt = g->transmission_type ? g->transmission_type : 'd';
        fmt = g->format ? g->format : RGBA;
        img = img_by_internal_id(self, self->loading_image);
        if (img == NULL) {
            self->loading_image = 0;
            ABRT(EILSEQ, "More payload loading refers to non-existent image");
        }
    }
    int fd;
    static char fname[2056] = {0};
    switch(tt) {
        case 'd':  // direct
            if (img->load_data.buf_capacity - img->load_data.buf_used < g->payload_sz) {
                if (img->load_data.buf_used + g->payload_sz > MAX_DATA_SZ || fmt != PNG) ABRT(EFBIG, "Too much data");
                img->load_data.buf_capacity = MIN(2 * img->load_data.buf_capacity, MAX_DATA_SZ);
                img->load_data.buf = realloc(img->load_data.buf, img->load_data.buf_capacity);
                if (img->load_data.buf == NULL) {
                    ABRT(ENOMEM, "Out of memory");
                    img->load_data.buf_capacity = 0; img->load_data.buf_used = 0;
                }
            }
            memcpy(img->load_data.buf + img->load_data.buf_used, payload, g->payload_sz);
            img->load_data.buf_used += g->payload_sz;
            if (!g->more) { img->data_loaded = true; self->loading_image = 0; }
            break;
        case 'f': // file
        case 't': // temporary file
        case 's': // POSIX shared memory
            if (g->payload_sz > 2048) ABRT(EINVAL, "Filename too long");
            snprintf(fname, sizeof(fname)/sizeof(fname[0]), "%.*s", (int)g->payload_sz, payload);
            if (tt == 's') fd = shm_open(fname, O_RDONLY, 0);
            else fd = open(fname, O_CLOEXEC | O_RDONLY);
            if (fd == -1) ABRT(EBADF, "Failed to open file %s for graphics transmission with error: [%d] %s", fname, errno, strerror(errno));
            img->data_loaded = mmap_img_file(self, img, fd, g->data_sz, g->data_offset);
            close(fd);
            if (tt == 't') unlink(fname);
            else if (tt == 's') shm_unlink(fname);
            break;
        default:
            ABRT(EINVAL, "Unknown transmission type: %c", g->transmission_type);
    }
    if (!img->data_loaded) return NULL;
    self->loading_image = 0;
    bool needs_processing = g->compressed || fmt == PNG;
    if (needs_processing) {
        uint8_t *buf; size_t bufsz;
#define IB { if (img->load_data.buf) { buf = img->load_data.buf; bufsz = img->load_data.buf_used; } else { buf = img->load_data.mapped_file; bufsz = img->load_data.mapped_file_sz; } }
        switch(g->compressed) {
            case 'z':
                IB;
                if (!inflate_zlib(self, img, buf, bufsz)) {
                    img->data_loaded = false; return NULL;
                }
                break;
            case 0:
                break;
            default:
                ABRT(EINVAL, "Unknown image compression: %c", g->compressed);
        }
        switch(fmt) {
            case PNG:
                IB;
                if (!inflate_png(self, img, buf, bufsz)) {
                    img->data_loaded = false; return NULL;
                }
                break;
            default: break;
        }
#undef IB
        img->load_data.data = img->load_data.buf;
        if (img->load_data.buf_used < img->load_data.data_sz) {
            ABRT(ENODATA, "Insufficient image data: %zu < %zu", img->load_data.buf_used, img->load_data.data_sz);
        }
        if (img->load_data.mapped_file) {
            munmap(img->load_data.mapped_file, img->load_data.mapped_file_sz);
            img->load_data.mapped_file = NULL; img->load_data.mapped_file_sz = 0;
        }
    } else {
        if (tt == 'd') {
            if (img->load_data.buf_used < img->load_data.data_sz) {
                ABRT(ENODATA, "Insufficient image data: %zu < %zu",  img->load_data.buf_used, img->load_data.data_sz);
            } else img->load_data.data = img->load_data.buf;
        } else {
            if (img->load_data.mapped_file_sz < img->load_data.data_sz) {
                ABRT(ENODATA, "Insufficient image data: %zu < %zu",  img->load_data.mapped_file_sz, img->load_data.data_sz);
            } else img->load_data.data = img->load_data.mapped_file;
        }
    }
    size_t required_sz = (img->load_data.is_opaque ? 3 : 4) * img->width * img->height;
    if (img->load_data.data_sz != required_sz) ABRT(EINVAL, "Image dimensions: %ux%u do not match data size: %zu, expected size: %zu", img->width, img->height, img->load_data.data_sz, required_sz);
    if (LIKELY(img->data_loaded && send_to_gpu)) {
        send_image_to_gpu(&img->texture_id, img->load_data.data, img->width, img->height, img->load_data.is_opaque, img->load_data.is_4byte_aligned);
        free_load_data(&img->load_data);
    }
    return img;
#undef MAX_DATA_SZ
#undef ABRT
}

static inline const char*
create_add_response(GraphicsManager UNUSED *self, const GraphicsCommand *g, bool data_loaded) {
    static char rbuf[sizeof(add_response)/sizeof(add_response[0])];
    if (g->id) {
        if (!has_add_respose) {
            if (!data_loaded) return NULL;
            snprintf(add_response, 10, "OK"); 
        }
        snprintf(rbuf, sizeof(rbuf)/sizeof(rbuf[0]) - 1, "\033_Gi=%u;%s\033\\", g->id, add_response);
        return rbuf;
    }
    return NULL;
}

// }}}

// Displaying images {{{

#define ensure_space_for(base, array, type, num, capacity, initial_cap) \
    if (base->capacity < num) { \
        base->capacity = MAX(initial_cap, MAX(2 * base->capacity, num)); \
        base->array = realloc(base->array, sizeof(type) * base->capacity); \
        if (base->array == NULL) fatal("Out of memory while ensuring space in array"); \
    }


static void 
handle_put_command(GraphicsManager *self, const GraphicsCommand *g, Cursor *c, bool *is_dirty, Image *img) {
    if (img == NULL) img = img_by_client_id(self, g->id);
    if (img == NULL) { REPORT_ERROR("Put command refers to non-existent image with id: %u", g->id); return; }
    if (!img->data_loaded) { REPORT_ERROR("Put command refers to image with id: %u that could not load its data", g->id); return; }
    ensure_space_for(img, refs, ImageRef, img->refcnt + 1, refcap, 10);
    *is_dirty = true;
    self->layers_dirty = true;
    ImageRef *ref = NULL;
    for (size_t i=0; i < img->refcnt; i++) {
        if ((unsigned)img->refs[i].start_row == c->x && (unsigned)img->refs[i].start_column == c->y) {
            ref = img->refs + i;
            break;
        }
    }
    if (ref == NULL) ref = img->refs + img->refcnt++;
    ref->src_x = g->x_offset; ref->src_y = g->y_offset; ref->src_width = g->width ? g->width : img->width; ref->src_height = g->height ? g->height : img->height;
    ref->src_width = MIN(ref->src_width, img->width - (img->width > ref->src_x ? ref->src_x : img->width));
    ref->src_height = MIN(ref->src_height, img->height - (img->height > ref->src_y ? ref->src_y : img->height));
    ref->z_index = g->z_index;
    ref->start_row = c->y; ref->start_column = c->x;
    ref->cell_x_offset = MIN(g->cell_x_offset, global_state.cell_width - 1);
    ref->cell_y_offset = MIN(g->cell_y_offset, global_state.cell_height - 1);
    ref->num_cols = g->num_cells; ref->num_rows = g->num_lines;

    // The src rect in OpenGL co-ords [0, 1] with origin at top-left corner of image
    ref->src_rect.left = (float)ref->src_x / (float)img->width;
    ref->src_rect.right = (float)(ref->src_x + ref->src_width) / (float)img->width;
    ref->src_rect.top = (float)ref->src_y / (float)img->height;
    ref->src_rect.bottom = (float)(ref->src_y + ref->src_height) / (float)img->height;

    // Move the cursor, the screen will take care of ensuring it is in bounds
    uint32_t num_cols = g->num_cells, num_rows = g->num_lines, t;
    if (num_cols == 0) {
        t = ref->src_width + ref->cell_x_offset;
        num_cols = t / global_state.cell_width;
        if (t > num_cols * global_state.cell_width) num_cols += 1;
    } 
    if (num_rows == 0) {
        t = ref->src_height + ref->cell_y_offset;
        num_rows = t / global_state.cell_height;
        if (t > num_rows * global_state.cell_height) num_rows += 1;
    } 
    c->x += num_cols; c->y += num_rows - 1;
}

static int 
cmp_by_zindex_and_image(const void *a_, const void *b_) { 
    const ImageRenderData *a = (const ImageRenderData*)a_, *b = (const ImageRenderData*)b_;
    int ans = a->z_index - b->z_index;
    if (ans == 0) ans = a->image_id - b->image_id;
    return ans; 
}

bool
grman_update_layers(GraphicsManager *self, unsigned int scrolled_by, float screen_left, float screen_top, float dx, float dy, unsigned int num_cols, unsigned int num_rows) {
    if (self->last_scrolled_by != scrolled_by) self->layers_dirty = true;
    self->last_scrolled_by = scrolled_by;
    if (!self->layers_dirty) return false;
    self->layers_dirty = false;
    size_t i, j;
    self->num_of_negative_refs = 0; self->num_of_positive_refs = 0;
    Image *img; ImageRef *ref;
    ImageRect r;
    float screen_width = dx * num_cols, screen_height = dy * num_rows;
    float screen_bottom = screen_top - screen_height;
    float screen_width_px = num_cols * global_state.cell_width;
    float screen_height_px = num_rows * global_state.cell_height;
    float y0 = screen_top - dy * scrolled_by;

    // Iterate over all visible refs and create render data
    self->count = 0;
    for (i = 0; i < self->image_count; i++) { img = self->images + i; for (j = 0; j < img->refcnt; j++) { ref = img->refs + j;
        r.top = y0 - ref->start_row * dy - dy * (float)ref->cell_y_offset / (float)global_state.cell_height;
        if (ref->num_rows) r.bottom = y0 - (ref->start_row + ref->num_rows) * dy;
        else r.bottom = r.top - screen_height * (float)ref->src_height / screen_height_px;
        if (r.top < screen_bottom || r.bottom > screen_top) continue;  // not visible

        r.left = screen_left + ref->start_column * dx + dx * (float)ref->cell_x_offset / (float) global_state.cell_width;
        if (ref->num_cols) r.right = screen_left + (ref->start_column + ref->num_cols) * dx;
        else r.right = r.left + screen_width * (float)ref->src_width / screen_width_px;

        if (ref->z_index < 0) self->num_of_negative_refs++; else self->num_of_positive_refs++;
        ensure_space_for(self, render_data, ImageRenderData, self->count + 1, capacity, 100);
        ImageRenderData *rd = self->render_data + self->count;
#define R(n, a, b) rd->vertices[n*4] = ref->src_rect.a; rd->vertices[n*4 + 1] = ref->src_rect.b; rd->vertices[n*4 + 2] = r.a; rd->vertices[n*4 + 3] = r.b;
        R(0, right, top); R(1, right, bottom); R(2, left, bottom); R(3, left, top);
#undef R
        self->count++;
        rd->z_index = ref->z_index; rd->image_id = img->internal_id;
        rd->texture_id = img->texture_id;
    }}
    if (!self->count) return false;
    // Sort visible refs in draw order (z-index, img)
    qsort(self->render_data, self->count, sizeof(ImageRenderData), cmp_by_zindex_and_image);
    // Calculate the group counts
    i = 0;
    while (i < self->count) {
        size_t image_id = self->render_data[i].image_id, start = i;
        if (start == self->count - 1) i = self->count;
        else {
            while (i < self->count - 1 && self->render_data[++i].image_id == image_id) {}
        }
        self->render_data[start].group_count = i - start;
    }
    return true;
}

// }}}

const char*
grman_handle_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, Cursor *c, bool *is_dirty) {
    Image *image;
    const char *ret = NULL;

    switch(g->action) {
        case 0:
        case 't':
        case 'T':
            image = handle_add_command(self, g, payload, is_dirty);
            ret = create_add_response(self, &self->last_init_graphics_command, image != NULL);
            if (self->last_init_graphics_command.action == 'T' && image && image->data_loaded) handle_put_command(self, &self->last_init_graphics_command, c, is_dirty, image);
            break;
        case 'p':
            if (!g->id) {
                REPORT_ERROR("%s", "Put graphics command without image id");
                break;
            }
            handle_put_command(self, g, c, is_dirty, NULL);
            break;
        default:
            REPORT_ERROR("Unknown graphics command action: %c", g->action);
            break;
    }
    return ret;
}

void
grman_clear(GraphicsManager UNUSED *self) {
    // TODO: Implement this
}


// Boilerplate {{{
static PyObject *
new(PyTypeObject UNUSED *type, PyObject *args, PyObject UNUSED *kwds) {
    unsigned int lines, columns;
    if (!PyArg_ParseTuple(args, "II", &lines, &columns)) return NULL;
    PyObject *ans = (PyObject*)grman_realloc(NULL, lines, columns);
    if (ans == NULL) PyErr_NoMemory();
    return ans;
}

static inline PyObject*
image_as_dict(Image *img) {
#define U(x) #x, img->x
    return Py_BuildValue("{sI sI sI sI sI sI sH sH sN}",
        U(texture_id), U(client_id), U(width), U(height), U(internal_id), U(refcnt), U(data_loaded),
        "is_4byte_aligned", img->load_data.is_4byte_aligned,
        "data", Py_BuildValue("y#", img->load_data.data, img->load_data.data_sz)
    );
#undef U

}

#define W(x) static PyObject* py##x(GraphicsManager UNUSED *self, PyObject *args)
#define PA(fmt, ...) if(!PyArg_ParseTuple(args, fmt, __VA_ARGS__)) return NULL;

W(image_for_client_id) {
    unsigned long id = PyLong_AsUnsignedLong(args);
    bool existing = false;
    Image *img = find_or_create_image(self, id, &existing);
    if (!existing) { Py_RETURN_NONE; }
    return image_as_dict(img);
}

W(shm_write) {
    const char *name, *data;
    Py_ssize_t sz;
    PA("ss#", &name, &data, &sz);
    int fd = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd == -1) { PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    int ret = ftruncate(fd, sz);
    if (ret != 0) { close(fd); PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    void *addr = mmap(0, sz, PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) { close(fd); PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    memcpy(addr, data, sz);
    if (munmap(addr, sz) != 0) { close(fd); PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    close(fd);
    Py_RETURN_NONE;
}

W(shm_unlink) {
    char *name;
    PA("s", &name);
    int ret = shm_unlink(name);
    if (ret == -1) { PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    Py_RETURN_NONE;
}

W(set_send_to_gpu) {
    send_to_gpu = PyObject_IsTrue(args) ? true : false;
    Py_RETURN_NONE;
}

W(update_layers) {
    unsigned int scrolled_by, sx, sy; float xstart, ystart, dx, dy;
    PA("IffffII", &scrolled_by, &xstart, &ystart, &dx, &dy, &sx, &sy);
    grman_update_layers(self, scrolled_by, xstart, ystart, dx, dy, sx, sy);
    PyObject *ans = PyTuple_New(self->count);
    for (size_t i = 0; i < self->count; i++) {
        ImageRenderData *r = self->render_data + i;
#define R(offset) Py_BuildValue("{sf sf sf sf}", "left", r->vertices[offset + 8], "top", r->vertices[offset + 1], "right", r->vertices[offset], "bottom", r->vertices[offset + 5])
        PyTuple_SET_ITEM(ans, i, 
            Py_BuildValue("{sN sN sI si sI}", "src_rect", R(0), "dest_rect", R(2), "group_count", r->group_count, "z_index", r->z_index, "image_id", r->image_id)
        );
#undef R
    }
    return ans;
}

#define M(x, va) {#x, (PyCFunction)py##x, va, ""}

static PyMethodDef methods[] = {
    M(image_for_client_id, METH_O),
    M(update_layers, METH_VARARGS),
    {NULL}  /* Sentinel */
};


PyTypeObject GraphicsManager_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.GraphicsManager",
    .tp_basicsize = sizeof(GraphicsManager),
    .tp_dealloc = (destructor)dealloc, 
    .tp_flags = Py_TPFLAGS_DEFAULT,        
    .tp_doc = "GraphicsManager",
    .tp_new = new,                
    .tp_methods = methods,
};

static PyMethodDef module_methods[] = {
    M(shm_write, METH_VARARGS),
    M(shm_unlink, METH_VARARGS),
    M(set_send_to_gpu, METH_O),
    {NULL, NULL, 0, NULL}        /* Sentinel */
};


bool
init_graphics(PyObject *module) {
    if (PyType_Ready(&GraphicsManager_Type) < 0) return false;
    if (PyModule_AddObject(module, "GraphicsManager", (PyObject *)&GraphicsManager_Type) != 0) return false; 
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    Py_INCREF(&GraphicsManager_Type);
    return true;
}
// }}}
