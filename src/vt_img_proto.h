#pragma once

#include "vt.h"
#include "vt_private.h"

#include "base64.h"
#include "stb_image/stb_image.h"

static const char* Vt_img_proto_display(Vt* self, uint32_t id, vt_image_proto_display_args_t args);

const char* Vt_img_proto_validate(Vt*                           self,
                                  vt_image_proto_transmission_t transmission_type,
                                  vt_image_proto_compression_t  compression_type,
                                  uint8_t                       format)
{
    if (Vt_client_host_is_local(self)) {
        switch (transmission_type) {
            case VT_IMAGE_PROTO_TRANSMISSION_DIRECT:
            case VT_IMAGE_PROTO_TRANSMISSION_FILE:
            case VT_IMAGE_PROTO_TRANSMISSION_TEMP_FILE:
                break;
            default:;
                return "transmission medium not supported";
        }
    } else {
        switch (transmission_type) {
            case VT_IMAGE_PROTO_TRANSMISSION_DIRECT:
                break;
            case VT_IMAGE_PROTO_TRANSMISSION_FILE:
            case VT_IMAGE_PROTO_TRANSMISSION_TEMP_FILE:
                return "client host is not local";
            default:;
                return "transmission medium not supported";
        }
    }

    switch (compression_type) {
        case VT_IMAGE_PROTO_COMPRESSION_NONE:
        case VT_IMAGE_PROTO_COMPRESSION_ZLIB: // TODO: actually test if this works right
            break;

        default:;
            return "compression method not supported";
    }

    switch (format) {
        case 24:
        case 32:
        case 100:
            break;
        default:
            return "image format not supported";
    }

    return NULL;
}

static stbi_uc* image_from_base64(char*     data,
                                  uint32_t* out_width,
                                  uint32_t* out_height,
                                  uint8_t*  out_bytes_per_pixel)
{
    size_t len     = strlen(data);
    size_t out_len = len * 3 / 4;
    char   decoded[out_len + 4];
    memset(decoded, 0, sizeof(decoded));
    base64_decode(data, decoded);

    int width, height, channels;
    stbi_info_from_memory((const stbi_uc*)decoded, out_len, &width, &height, &channels);
    stbi_uc* pixels =
      stbi_load_from_memory((const stbi_uc*)decoded, out_len, &width, &height, &channels, channels);

    if (out_width) {
        *out_width = width;
    }

    if (out_height) {
        *out_height = height;
    }

    *out_bytes_per_pixel = channels;

    return pixels;
}

static void maybe_unlink_tmp_file(const char* name)
{
    if (is_in_tmp_dir(name)) {
        LOG("Vt::img_proto::unlink_tmp_file{ %s }\n", name);
        unlink(name);
    } else {
        WRN("Temporary image file \'%s\' used for transmission is not located in a known temporary "
            "directory and will NOT be unliked\n",
            name);
    }
}

static stbi_uc* image_from_base64_file_name(char*     file_name,
                                            size_t    opt_size,
                                            size_t    opt_offset,
                                            uint32_t* out_width,
                                            uint32_t* out_height,
                                            uint8_t*  out_bytes_per_pixel,
                                            bool      tmp)
{
    size_t len = strlen(file_name);
    char   decoded_name[len * 3 / 4 + 4];
    decoded_name[base64_decode(file_name, decoded_name)] = '\0';

    LOG("Vt::img_proto::read_image_file{ %s }\n", decoded_name);
    FILE* file = fopen(decoded_name, "rb");

    if (!file) {
        WRN("Failed to open file \'%s\', %s\n", decoded_name, strerror(errno));
        return NULL;
    }

    fseek(file, opt_offset, SEEK_SET);

    int width, height, channels;
    stbi_info_from_file(file, &width, &height, &channels);
    stbi_uc* pixels = stbi_load_from_file(file, &width, &height, &channels, channels);
    fclose(file);

    LOG("Vt::img_proto::read_image_file{ name: %s, dims: %dx%d, channels: %d }\n",
        decoded_name,
        width,
        height,
        channels);

    if (tmp) {
        maybe_unlink_tmp_file(decoded_name);
    }

    if (out_width) {
        *out_width = width;
    }

    if (out_height) {
        *out_height = height;
    }

    *out_bytes_per_pixel = channels;

    return pixels;
}

static uint8_t* data_from_base64_file_name(char*                        file_name,
                                           vt_image_proto_compression_t compression_type,
                                           size_t                       opt_size,
                                           size_t                       opt_offset,
                                           bool                         tmp)
{
    size_t len = strlen(file_name);
    char   decoded_name[len * 3 / 4 + 4];
    base64_decode(file_name, decoded_name);

    LOG("Vt::img_proto::read_raw_file{ %s }\n", decoded_name);
    FILE* file = fopen(decoded_name, "rb");

    if (!file) {
        WRN("Failed to open file \'%s\', %s\n", decoded_name, strerror(errno));
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    size_t size_file = ftell(file);
    opt_size         = OR(opt_size, (size_file - opt_offset));
    uint8_t* pixels  = malloc(opt_size);
    fseek(file, opt_offset, SEEK_SET);
    size_t rd = fread(pixels, 1, opt_size, file);
    fclose(file);

    if (!rd) {
        WRN("failed to read from file\n");
        free(pixels);
        return NULL;

        if (tmp) {
            maybe_unlink_tmp_file(decoded_name);
        }
    }

    switch (compression_type) {
        case VT_IMAGE_PROTO_COMPRESSION_ZLIB: {
            uint8_t* px =
              (void*)stbi_zlib_decode_malloc_guesssize((void*)pixels, opt_size, opt_size / 2, NULL);
            free(pixels);
            pixels = px;
        } break;
        default:;
    }

    if (tmp) {
        maybe_unlink_tmp_file(decoded_name);
    }

    return pixels;
}

static uint8_t* data_from_base64(uint8_t* input, vt_image_proto_compression_t compression_type)
{
    size_t   len     = strlen((char*)input);
    size_t   out_len = len * 3 / 4;
    uint8_t* pixels  = calloc(1, out_len + 4);
    base64_decode((char*)input, (char*)pixels);
    return pixels;
}

static RcPtr_VtImageSurface* Vt_get_image_surface_rp(Vt* self, uint32_t id)
{
    RcPtr_VtImageSurface* surface = NULL;
    if (id) {
        for (RcPtr_VtImageSurface* i = NULL;
             (i = Vector_iter_RcPtr_VtImageSurface(&self->images, i));) {
            VtImageSurface* s = RcPtr_get_VtImageSurface(i);
            if (s && s->id == id) {
                surface = i;
                break;
            }
        }
    }

    if (!surface && RcPtr_get_VtImageSurface(&self->manipulated_image)) {
        surface = &self->manipulated_image;
    }

    return surface;
}

static VtImageSurface* Vt_get_image_surface(Vt* self, uint32_t id)
{
    VtImageSurface* surface = NULL;

    if (id) {
        for (RcPtr_VtImageSurface* i = NULL;
             (i = Vector_iter_RcPtr_VtImageSurface(&self->images, i));) {
            VtImageSurface* s = RcPtr_get_VtImageSurface(i);
            if (s && s->id == id) {
                surface = s;
                break;
            }
        }
    }

    if (!surface) {
        surface = RcPtr_get_VtImageSurface(&self->manipulated_image);
    }

    return surface;
}

const char* Vt_img_proto_transmit(Vt*                           self,
                                  vt_image_proto_transmission_t transmission_type,
                                  vt_image_proto_compression_t  compression_type,
                                  uint8_t                       format,
                                  bool                          is_complete,
                                  size_t                        offset,
                                  size_t                        size,
                                  vt_image_proto_display_args_t display_args,
                                  bool                          queue_display,
                                  uint32_t                      id,
                                  uint32_t                      width,
                                  uint32_t                      height,
                                  char*                         payload)
{
    const char*     fail_msg = NULL;
    VtImageSurface* surface  = Vt_get_image_surface(self, id);

    if (surface && surface->state != VT_IMAGE_SURFACE_INCOMPLETE) {
        Vector_clear_uint8_t(&surface->fragments);
        CALL_FP(self->callbacks.destroy_image_proxy, self->callbacks.user_data, &surface->proxy);
        surface->state = VT_IMAGE_SURFACE_INCOMPLETE;
    }

    if (!surface) {
        VtImageSurface srf;
        srf.fragments = Vector_new_uint8_t();
        srf.state     = VT_IMAGE_SURFACE_INCOMPLETE;
        srf.id        = id;
        memset(&srf.proxy, 0, sizeof(srf.proxy));

        RcPtr_VtImageSurface rc        = RcPtr_new_VtImageSurface(self);
        *RcPtr_get_VtImageSurface(&rc) = srf;

        if (id) {
            Vector_push_RcPtr_VtImageSurface(&self->images, rc);
            surface = RcPtr_get_VtImageSurface(Vector_last_RcPtr_VtImageSurface(&self->images));
        } else {
            RcPtr_destroy_VtImageSurface(&self->manipulated_image);
            self->manipulated_image = rc;
            surface                 = RcPtr_get_VtImageSurface(&self->manipulated_image);
        }
    }

    switch (transmission_type) {
        case VT_IMAGE_PROTO_TRANSMISSION_DIRECT: {
            if (queue_display) {
                surface->display_on_transmission_completed = true;
                surface->display_args                      = display_args;
            }

            if (format == 100) {
                surface->png_data_transmission = true;
            }

            Vector_pushv_uint8_t(&surface->fragments, (uint8_t*)payload, strlen(payload));

            if (is_complete) {
                Vector_push_uint8_t(&surface->fragments, 0);

                if (!surface->png_data_transmission) {
                    uint8_t* data = data_from_base64(surface->fragments.buf, compression_type);

                    if (data) {
                        surface->state = VT_IMAGE_SURFACE_READY;
                        Vector_clear_uint8_t(&surface->fragments);
                        Vector_pushv_uint8_t(&surface->fragments,
                                             data,
                                             surface->width * surface->height *
                                               surface->bytes_per_pixel);
                        free(data);
                        if (surface->display_on_transmission_completed) {
                            Vt_img_proto_display(self, id, surface->display_args);
                            if (!id) {
                                RcPtr_destroy_VtImageSurface(&self->manipulated_image);
                            }
                        }
                    } else {
                        fail_msg = "image format error";
                    }
                } else {
                    stbi_uc* image = image_from_base64((char*)surface->fragments.buf,
                                                       &surface->width,
                                                       &surface->height,
                                                       &surface->bytes_per_pixel);
                    if (image) {
                        surface->state = VT_IMAGE_SURFACE_READY;
                        Vector_clear_uint8_t(&surface->fragments);
                        Vector_pushv_uint8_t(&surface->fragments,
                                             (uint8_t*)image,
                                             surface->width * surface->height *
                                               surface->bytes_per_pixel);
                        stbi_image_free(image);

                        if (surface->display_on_transmission_completed) {
                            Vt_img_proto_display(self, id, surface->display_args);

                            if (!id) {
                                RcPtr_destroy_VtImageSurface(&self->manipulated_image);
                            }
                        }

                    } else {
                        fail_msg = "image format error";
                    }
                }

            } else {
                if (width) {
                    surface->width = width;
                }

                if (height) {
                    surface->height = height;
                }
            }
        } break;

        case VT_IMAGE_PROTO_TRANSMISSION_TEMP_FILE:
        case VT_IMAGE_PROTO_TRANSMISSION_FILE: {
            if (queue_display) {
                surface->display_on_transmission_completed = true;
                surface->display_args                      = display_args;
            }

            bool     is_raw_pixel_data = format != 100;
            stbi_uc* image             = NULL;
            uint8_t* raw_image         = NULL;

            if (is_raw_pixel_data) {
                surface->bytes_per_pixel = 4;

                raw_image = data_from_base64_file_name(payload,
                                                       compression_type,
                                                       size,
                                                       offset,
                                                       transmission_type ==
                                                         VT_IMAGE_PROTO_TRANSMISSION_TEMP_FILE);
                if (width) {
                    surface->width = width;
                }

                if (height) {
                    surface->height = height;
                }
            } else {
                surface->bytes_per_pixel = 4;

                image = image_from_base64_file_name(payload,
                                                    size,
                                                    offset,
                                                    &surface->width,
                                                    &surface->height,
                                                    &surface->bytes_per_pixel,
                                                    transmission_type ==
                                                      VT_IMAGE_PROTO_TRANSMISSION_TEMP_FILE);
            }

            if (image || raw_image) {
                surface->state = VT_IMAGE_SURFACE_READY;
                Vector_pushv_uint8_t(&surface->fragments,
                                     is_raw_pixel_data ? raw_image : image,
                                     surface->width * surface->height * surface->bytes_per_pixel);
                stbi_image_free(image);
                free(raw_image);

                if (surface->display_on_transmission_completed) {
                    Vt_img_proto_display(self, id, surface->display_args);
                    if (!id) {
                        RcPtr_destroy_VtImageSurface(&self->manipulated_image);
                    }
                }
            } else {
                fail_msg = "image format error";
            }

        } break;

        default:
            fail_msg = "transmission medium not supported";
    }

    if (fail_msg) {
        Vector_clear_uint8_t(&surface->fragments);
        surface->state = VT_IMAGE_SURFACE_FAIL;
    }

    return fail_msg;
}

static bool VtImageSurfaceView_spans_column(VtImageSurfaceView* self, uint16_t col)
{
    return (self->anchor_cell_idx <= col) && (self->anchor_cell_idx + self->cell_size.first >= col);
}

static bool VtImageSurfaceView_spans_line(VtImageSurfaceView* self, size_t idx)
{
    return self->anchor_global_index <= idx &&
           (self->anchor_global_index + self->cell_size.second) >= idx;
}

static bool Vt_ImageSurfaceView_is_visible(const Vt* self, VtImageSurfaceView* view)
{
    return Vt_top_line(self) <= view->anchor_global_index + view->cell_size.second;
}

static bool VtImageSurfaceView_intersects(VtImageSurfaceView* self, size_t idx, uint16_t col)
{
    return VtImageSurfaceView_spans_line(self, idx) && VtImageSurfaceView_spans_column(self, col);
}

static void Vt_crop_VtImageSurfaceView_bottom_by_line(Vt* self, VtImageSurfaceView* view)
{
    --view->cell_size.second;
    view->sample_dims_px.second -= self->pixels_per_cell_y;
    CALL_FP(self->callbacks.destroy_image_view_proxy, self->callbacks.user_data, &view->proxy);
}

static VtImageSurfaceView Vt_crop_VtImageSurfaceView_top_by_line(Vt* self, VtImageSurfaceView* view)
{
    VtImageSurfaceView image_view;
    memcpy(&image_view, view, sizeof(image_view));
    memset(&image_view.proxy, 0, sizeof(image_view.proxy));
    image_view.source_image_surface = RcPtr_new_shared_VtImageSurface(&view->source_image_surface);
    --image_view.cell_size.second;
    image_view.sample_dims_px.second -= self->pixels_per_cell_y;
    image_view.sample_offset_px.second += self->pixels_per_cell_y;

    return image_view;
}

static void Vt_recalculate_VtImageSurfaceView_dimensions(Vt* self, VtImageSurfaceView* view)
{
    VtImageSurface* src = RcPtr_get_VtImageSurface(&view->source_image_surface);

    if (!(view->cell_size.first = view->cell_scale_rect.first)) {
        int32_t image_width =
          view->anchor_offset_px.first +
          (view->sample_dims_px.first ? view->sample_dims_px.first : src->width);
        view->cell_size.first = image_width / self->pixels_per_cell_x;
    }

    if (!(view->cell_size.second = view->cell_scale_rect.second)) {
        int32_t image_height =
          view->anchor_offset_px.second +
          (view->sample_dims_px.second ? view->sample_dims_px.second : src->height);
        view->cell_size.second = image_height / self->pixels_per_cell_y;
    }
}

static const char* Vt_img_proto_display(Vt* self, uint32_t id, vt_image_proto_display_args_t args)
{
    RcPtr_VtImageSurface* source = Vt_get_image_surface_rp(self, id);

    if (!source) {
        return "no such id";
    }

    VtImageSurface* src = RcPtr_get_VtImageSurface(source);

    if (!src || !src->width || !src->height || src->state == VT_IMAGE_SURFACE_INCOMPLETE) {
        return "source transmission incomplete";
    }

    if (src->state == VT_IMAGE_SURFACE_DESTROYED) {
        return id ? "source explicitly deleted by client" : "source deleted";
    }

    if (src->state == VT_IMAGE_SURFACE_FAIL) {
        return "source transmission failed";
    }

    uint16_t anchor_cell = self->cursor.col;
    VtLine*  ln          = Vt_cursor_line(self);

    if (!ln->graphic_attachments) {
        ln->graphic_attachments = calloc(1, sizeof(VtGraphicLineAttachments));
    }

    if (!ln->graphic_attachments->images) {
        ln->graphic_attachments->images  = malloc(sizeof(Vector_RcPtr_VtImageSurfaceView));
        *ln->graphic_attachments->images = Vector_new_RcPtr_VtImageSurfaceView();
    }

    VtImageSurfaceView image_view;
    image_view.anchor_global_index     = self->cursor.row;
    image_view.anchor_cell_idx         = anchor_cell;
    image_view.anchor_offset_px.first  = args.anchor_offset_x;
    image_view.anchor_offset_px.second = args.anchor_offset_y;
    image_view.z_layer                 = args.z_layer;
    image_view.cell_scale_rect.first   = args.cell_width;
    image_view.cell_scale_rect.second  = args.cell_height;
    image_view.sample_offset_px.first  = args.sample_offset_x;
    image_view.sample_offset_px.second = args.sample_offset_y;
    image_view.sample_dims_px.first    = args.sample_width;
    image_view.sample_dims_px.second   = args.sample_height;
    image_view.source_image_surface    = RcPtr_new_shared_VtImageSurface(source);

    memset(&image_view.proxy, 0, sizeof(image_view.proxy));
    Vt_recalculate_VtImageSurfaceView_dimensions(self, &image_view);

    RcPtr_VtImageSurfaceView iv_ptr        = RcPtr_new_VtImageSurfaceView(self);
    *RcPtr_get_VtImageSurfaceView(&iv_ptr) = image_view;
    RcPtr_VtImageSurfaceView iv_ptr2       = RcPtr_new_shared_VtImageSurfaceView(&iv_ptr);

    Vector_push_RcPtr_VtImageSurfaceView(ln->graphic_attachments->images, iv_ptr);
    Vector_push_RcPtr_VtImageSurfaceView(&self->image_views, iv_ptr2);

    for (int i = 1; i < image_view.cell_size.second; ++i) {
        Vt_insert_new_line(self);
    }

    Vt_move_cursor(self, self->cursor.col + image_view.cell_size.first, Vt_cursor_row(self));

    return NULL;
}
