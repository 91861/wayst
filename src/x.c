/* See LICENSE for license information. */

#ifndef NOX // if x11 support enabled at compile time

#define _GNU_SOURCE

#include "x.h"
#include "vector.h"

#include <X11/Xlib.h>
#include <uchar.h>

#include <GL/glx.h>
#include <X11/X.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysymdef.h>

#define _NET_WM_STATE_REMOVE 0l
#define _NET_WM_STATE_ADD    1l
#define _NET_WM_STATE_TOGGLE 2l

#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_SWAP_INTERVAL_EXT         0x20F1
#define GLX_MAX_SWAP_INTERVAL_EXT     0x20F2

static APIENTRY PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT = NULL;

static int32_t convert_modifier_mask(unsigned int x_mask)
{
    int32_t mods = 0;
    if (x_mask & ShiftMask) {
        FLAG_SET(mods, MODIFIER_SHIFT);
    }
    if (x_mask & ControlMask) {
        FLAG_SET(mods, MODIFIER_CONTROL);
    }
    if (x_mask & Mod1Mask) {
        FLAG_SET(mods, MODIFIER_CONTROL);
    }
    return mods;
}

static WindowStatic* global;

#define globalX11       ((GlobalX11*)&global->extend_data)
#define windowX11(base) ((WindowX11*)&base->extend_data)

static struct WindowBase* WindowX11_new(uint32_t w, uint32_t h);
static void               WindowX11_set_fullscreen(struct WindowBase* self, bool fullscreen);
static void               WindowX11_set_maximized(struct WindowBase* self, bool maximized);
static void               WindowX11_resize(struct WindowBase* self, uint32_t w, uint32_t h);
static void               WindowX11_events(struct WindowBase* self);
static void     WindowX11_set_wm_name(struct WindowBase* self, const char* title, const char* name);
static void     WindowX11_set_title(struct WindowBase* self, const char* title);
static void     WindowX11_set_swap_interval(struct WindowBase* self, int32_t ival);
static bool     WindowX11_maybe_swap(struct WindowBase* self);
static void     WindowX11_destroy(struct WindowBase* self);
static int      WindowX11_get_connection_fd(struct WindowBase* self);
static void     WindowX11_clipboard_get(struct WindowBase* self);
static void     WindowX11_clipboard_send(struct WindowBase* self, const char* text);
static void     WindowX11_set_pointer_style(struct WindowBase* self, enum MousePointerStyle style);
static void*    WindowX11_get_gl_ext_proc_adress(struct WindowBase* self, const char* name);
static uint32_t WindowX11_get_keycode_from_name(struct WindowBase* self, char* name);
static void     WindowX11_set_current_context(struct WindowBase* self, bool this);
static inline void WindowX11_set_urgent(struct WindowBase* self);
static inline void WindowX11_set_stack_order(struct WindowBase* self, bool front_or_back);
static TimePoint*  WindowX11_process_timers(struct WindowBase* self)
{
    return NULL;
}

static struct IWindow window_interface_x11 = {
    .set_fullscreen         = WindowX11_set_fullscreen,
    .set_maximized          = WindowX11_set_maximized,
    .resize                 = WindowX11_resize,
    .events                 = WindowX11_events,
    .process_timers         = WindowX11_process_timers,
    .set_title              = WindowX11_set_title,
    .maybe_swap             = WindowX11_maybe_swap,
    .destroy                = WindowX11_destroy,
    .get_connection_fd      = WindowX11_get_connection_fd,
    .clipboard_send         = WindowX11_clipboard_send,
    .clipboard_get          = WindowX11_clipboard_get,
    .set_swap_interval      = WindowX11_set_swap_interval,
    .get_gl_ext_proc_adress = WindowX11_get_gl_ext_proc_adress,
    .get_keycode_from_name  = WindowX11_get_keycode_from_name,
    .set_pointer_style      = WindowX11_set_pointer_style,
    .set_current_context    = WindowX11_set_current_context,
    .set_urgent             = WindowX11_set_urgent,
    .set_stack_order        = WindowX11_set_stack_order,
};

typedef struct
{
    Display* display;
    Atom     wm_delete;

    Cursor cursor_hidden;
    Cursor cursor_beam;
    Cursor cursor_hand;

    XIM im;
    XIC ic;
} GlobalX11;

typedef struct
{
    Window               window;
    GLXContext           glx_context;
    XEvent               event;
    XSetWindowAttributes set_win_attribs;
    Colormap             colormap;
    uint32_t             last_button_pressed;
    const char*          cliptext;
} WindowX11;

static void WindowX11_clipboard_send(struct WindowBase* self, const char* text)
{
    if (windowX11(self)->cliptext)
        free((void*)windowX11(self)->cliptext);

    Atom sel                  = XInternAtom(globalX11->display, "CLIPBOARD", False);
    windowX11(self)->cliptext = text;
    XSetSelectionOwner(globalX11->display, sel, windowX11(self)->window, CurrentTime);
}

static void WindowX11_clipboard_get(struct WindowBase* self)
{
    Atom   clip  = XInternAtom(globalX11->display, "CLIPBOARD", 0);
    Atom   utf8  = XInternAtom(globalX11->display, "UTF8_STRING", 0);
    Window owner = XGetSelectionOwner(globalX11->display, clip);

    if (owner != None) {
        XConvertSelection(globalX11->display,
                          clip,
                          utf8,
                          clip,
                          windowX11(self)->window,
                          CurrentTime);
    }
}

static void WindowX11_setup_pointer(struct WindowBase* self)
{
    XColor      c       = { .red = 0, .green = 0, .blue = 0 };
    static char data[8] = { 0 };
    Pixmap pmp = XCreateBitmapFromData(globalX11->display, windowX11(self)->window, data, 8, 8);
    globalX11->cursor_hidden = XCreatePixmapCursor(globalX11->display, pmp, pmp, &c, &c, 0, 0);
    globalX11->cursor_beam   = XCreateFontCursor(globalX11->display, XC_xterm);
    globalX11->cursor_hand   = XCreateFontCursor(globalX11->display, XC_hand1);
}

static void* WindowX11_get_gl_ext_proc_adress(struct WindowBase* self, const char* name)
{
    return glXGetProcAddress((const GLubyte*)name);
}

static struct WindowBase* WindowX11_new(uint32_t w, uint32_t h)
{
    if (!global) {
        global = calloc(1, sizeof(WindowStatic) + sizeof(GlobalX11) - sizeof(uint8_t));
    }

    globalX11->display = XOpenDisplay(NULL);
    if (!globalX11->display) {
        free(global);
        return NULL;
    }

    int glx_major, glx_minor, qry_res;
    if (!(qry_res = glXQueryVersion(globalX11->display, &glx_major, &glx_minor)) ||
        (glx_major == 1 && glx_minor < 3)) {
        WRN("GLX version to low\n");
        free(global);
        return NULL;
    }

    if (!XSupportsLocale()) {
        ERR("Xorg does not support locales\n");
    }

    struct WindowBase* win =
      calloc(1, sizeof(struct WindowBase) + sizeof(WindowX11) - sizeof(uint8_t));

    XSetLocaleModifiers("@im=none");
    globalX11->im = XOpenIM(globalX11->display, NULL, NULL, NULL);
    if (!globalX11->im) {
        ERR("Failed to open input method");
    }
    globalX11->ic = XCreateIC(globalX11->im,
                              XNInputStyle,
                              XIMPreeditNothing | XIMStatusNothing,
                              XNClientWindow,
                              windowX11(win)->window,
                              NULL);
    if (!globalX11->ic) {
        ERR("Failed to create input context");
    }
    XSetICFocus(globalX11->ic);

    win->w                            = w;
    win->h                            = h;
    win->interface                    = &window_interface_x11;
    static const int visual_attribs[] = { GLX_RENDER_TYPE,
                                          GLX_RGBA_BIT,
                                          GLX_DRAWABLE_TYPE,
                                          GLX_WINDOW_BIT,
                                          GLX_DOUBLEBUFFER,
                                          True,
                                          GLX_RED_SIZE,
                                          1,
                                          GLX_GREEN_SIZE,
                                          1,
                                          GLX_BLUE_SIZE,
                                          1,
                                          GLX_ALPHA_SIZE,
                                          1,
                                          GLX_DEPTH_SIZE,
                                          GLX_DONT_CARE,
                                          None };

    int          framebuffer_config_count;
    GLXFBConfig* framebuffer_configs = glXChooseFBConfig(globalX11->display,
                                                         DefaultScreen(globalX11->display),
                                                         visual_attribs,
                                                         &framebuffer_config_count);
    if (!framebuffer_configs) {
        ERR("glXChooseFBConfig failed");
    }

    XVisualInfo* visual_info = NULL;

    int framebuffer_config_selected_idx          = -1;
    int framebuffer_config_selected_no_alpha_idx = -1;

    for (int i = 0; i < framebuffer_config_count; ++i) {
        visual_info = glXGetVisualFromFBConfig(globalX11->display, framebuffer_configs[i]);
        if (!visual_info) {
            continue;
        }

        XRenderPictFormat* visual_pict_format =
          XRenderFindVisualFormat(globalX11->display, visual_info->visual);

        if (!visual_pict_format) {
            continue;
        }

        LOG("X::Visual picture format{depth: %d, r:%d(%d), g:%d(%d), b:%d(%d), a:%d(%d), type: %d, "
            "pf id: %lu}\n",
            visual_pict_format->depth,
            visual_pict_format->direct.red,
            visual_pict_format->direct.redMask,
            visual_pict_format->direct.green,
            visual_pict_format->direct.greenMask,
            visual_pict_format->direct.blue,
            visual_pict_format->direct.blueMask,
            visual_pict_format->direct.alpha,
            visual_pict_format->direct.alphaMask,
            visual_pict_format->type,
            visual_pict_format->id);

        if (visual_pict_format->direct.redMask > 0 && visual_pict_format->direct.greenMask > 0 &&
            visual_pict_format->direct.blueMask > 0 && visual_pict_format->depth >= 24) {

            if (visual_pict_format->direct.alphaMask > 0 && visual_pict_format->depth >= 32) {
                framebuffer_config_selected_idx = i;
                break;
            } else {
                framebuffer_config_selected_no_alpha_idx = i;
            }
        }

        XFree(visual_info);
        visual_info = NULL;
    }

    if (framebuffer_config_selected_idx < 0) {
        WRN("No transparent framebuffer found\n");
        framebuffer_config_selected_idx = framebuffer_config_selected_no_alpha_idx;
    }

    if (framebuffer_config_selected_idx < 0) {
        ERR("No suitable framebuffer configuration found");
    }

    if (!visual_info) {
        visual_info =
          glXGetVisualFromFBConfig(globalX11->display,
                                   framebuffer_configs[framebuffer_config_selected_idx]);
        if (!visual_info) {
            ERR("Failed to get visual info");
        }
    }

    Colormap colormap = XCreateColormap(globalX11->display,
                                        RootWindow(globalX11->display, visual_info->screen),
                                        visual_info->visual,
                                        AllocNone);

    long event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                      SubstructureRedirectMask | StructureNotifyMask | PointerMotionMask |
                      ExposureMask | FocusChangeMask | KeymapStateMask | VisibilityChangeMask;

    windowX11(win)->set_win_attribs = (XSetWindowAttributes){
        .colormap = windowX11(win)->colormap = colormap,
        .border_pixel                        = 0,
        .background_pixmap                   = None,
        .override_redirect                   = True,
        .event_mask                          = event_mask,
    };

    windowX11(win)->glx_context = NULL;

    const char* exts =
      glXQueryExtensionsString(globalX11->display, DefaultScreen(globalX11->display));
    LOG("X::GLX extensions: %s\n", exts);

    static const int context_attrs[] = { GLX_CONTEXT_MAJOR_VERSION_ARB,
                                         2,
                                         GLX_CONTEXT_MINOR_VERSION_ARB,
                                         1,
                                         None };

    if (strstr(exts, "_swap_control")) {
        glXSwapIntervalEXT = (APIENTRY PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddressARB(
          (const GLubyte*)"glXSwapIntervalEXT");
    }
    if (!glXSwapIntervalEXT) {
        WRN("glXSwapIntervalEXT not found\n");
    }

    APIENTRY PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB = NULL;
    if (strstr(exts, "GLX_ARB_create_context")) {
        glXCreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB(
          (const GLubyte*)"glXCreateContextAttribsARB");
    }

    if (!glXCreateContextAttribsARB) {
        WRN("glXCreateContextAttribsARB not found\n");
        windowX11(win)->glx_context =
          glXCreateNewContext(globalX11->display,
                              framebuffer_configs[framebuffer_config_selected_idx],
                              GLX_RGBA_TYPE,
                              0,
                              True);
    } else {
        windowX11(win)->glx_context =
          glXCreateContextAttribsARB(globalX11->display,
                                     framebuffer_configs[framebuffer_config_selected_idx],
                                     0,
                                     True,
                                     context_attrs);
    }

    if (!windowX11(win)->glx_context) {
        ERR("Failed to create GLX context");
    }

    windowX11(win)->window = XCreateWindow(globalX11->display,
                                           RootWindow(globalX11->display, visual_info->screen),
                                           0,
                                           0,
                                           win->w,
                                           win->h,
                                           0,
                                           visual_info->depth,
                                           InputOutput,
                                           visual_info->visual,
                                           CWBorderPixel | CWColormap | CWEventMask,
                                           &windowX11(win)->set_win_attribs);
    if (!windowX11(win)->window) {
        ERR("Failed to create X11 window");
    }

    XFree(framebuffer_configs);
    XFree(visual_info);

    Atom win_type_normal = XInternAtom(globalX11->display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(globalX11->display,
                    windowX11(win)->window,
                    XInternAtom(globalX11->display, "_NET_WM_WINDOW_TYPE", False),
                    XA_ATOM,
                    32,
                    PropModeReplace,
                    (unsigned char*)&win_type_normal,
                    1);

    /* XChangeProperty(globalX11->display, */
    /*                 windowX11(win)->window, */
    /*                 XInternAtom(globalX11->display, "_NET_WM_ICON_NAME", False), */
    /*                 XInternAtom(globalX11->display, "UTF8_STRING", False), */
    /*                 8, */
    /*                 PropModeReplace, */
    /*                 (unsigned char*)"wayst", */
    /*                 strlen("wayst")); */

    /* XSetIconName(globalX11->display, windowX11(win)->window, "wayst"); */

    XClassHint class_hint = { APPLICATION_NAME, "CLASS" };
    XWMHints   wm_hints   = { .flags = InputHint, .input = True };
    XSetWMProperties(globalX11->display,
                     windowX11(win)->window,
                     NULL,
                     NULL,
                     NULL,
                     0,
                     NULL,
                     &wm_hints,
                     &class_hint);

    XSync(globalX11->display, False);
    XMapWindow(globalX11->display, windowX11(win)->window);
    glXMakeCurrent(globalX11->display, windowX11(win)->window, windowX11(win)->glx_context);
    globalX11->wm_delete = XInternAtom(globalX11->display, "WM_DELETE_WINDOW", True);
    XSetWMProtocols(globalX11->display, windowX11(win)->window, &globalX11->wm_delete, 1);
    WindowX11_setup_pointer(win);
    XkbSelectEvents(globalX11->display, XkbUseCoreKbd, XkbAllEventsMask, XkbAllEventsMask);
    XFlush(globalX11->display);

    return win;
}

struct WindowBase* Window_new_x11(Pair_uint32_t res)
{
    struct WindowBase* win = WindowX11_new(res.first, res.second);

    if (!win)
        return NULL;

    win->title = NULL;
    WindowX11_set_wm_name(win,
                          OR(settings.user_app_id, APPLICATION_NAME),
                          OR(settings.user_app_id_2, NULL));
    WindowX11_set_title(win, settings.title.str);

    return win;
}

static inline void WindowX11_set_urgent(struct WindowBase* self)
{
    XEvent e               = { 0 };
    e.type                 = ClientMessage;
    e.xclient.window       = windowX11(self)->window;
    e.xclient.message_type = XInternAtom(globalX11->display, "_NET_WM_STATE", True);
    e.xclient.format       = 32;
    e.xclient.data.l[0]    = _NET_WM_STATE_ADD;
    e.xclient.data.l[1] = XInternAtom(globalX11->display, "_NET_WM_STATE_DEMANDS_ATTENTION", True);
    e.xclient.data.l[3] = 0;

    XSendEvent(globalX11->display,
               DefaultRootWindow(globalX11->display),
               False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               (XEvent*)&e);
    XFlush(globalX11->display);
}

static inline void WindowX11_fullscreen_change_state(struct WindowBase* self, const long arg)
{
    XEvent e               = { 0 };
    e.type                 = ClientMessage;
    e.xclient.window       = windowX11(self)->window;
    e.xclient.message_type = XInternAtom(globalX11->display, "_NET_WM_STATE", True);
    e.xclient.format       = 32;
    e.xclient.data.l[0]    = (long)arg;
    e.xclient.data.l[1]    = XInternAtom(globalX11->display, "_NET_WM_STATE_FULLSCREEN", True);
    e.xclient.data.l[3]    = 0;

    XSendEvent(globalX11->display,
               DefaultRootWindow(globalX11->display),
               False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               (XEvent*)&e);
    XFlush(globalX11->display);
}

static inline void WindowX11_set_stack_order(struct WindowBase* self, bool front_or_back)
{
    if (front_or_back) {
        XRaiseWindow(globalX11->display, windowX11(self)->window);
    } else {
        XLowerWindow(globalX11->display, windowX11(self)->window);
    }
}

static void WindowX11_set_fullscreen(struct WindowBase* self, bool fullscreen)
{
    if (fullscreen && !FLAG_IS_SET(self->state_flags, WINDOW_IS_FULLSCREEN)) {
        WindowX11_fullscreen_change_state(self, _NET_WM_STATE_ADD);
        FLAG_SET(self->state_flags, WINDOW_IS_FULLSCREEN);
    } else if (!fullscreen && FLAG_IS_SET(self->state_flags, WINDOW_IS_FULLSCREEN)) {
        WindowX11_fullscreen_change_state(self, _NET_WM_STATE_REMOVE);
        FLAG_UNSET(self->state_flags, WINDOW_IS_FULLSCREEN);
    }
}

static void WindowX11_set_maximized(struct WindowBase* self, bool maximized)
{
    if (maximized) {
        WindowX11_set_fullscreen(self, false);
        FLAG_SET(self->state_flags, WINDOW_IS_MAXIMIZED);
    } else {
        FLAG_UNSET(self->state_flags, WINDOW_IS_MAXIMIZED);
    }

    XEvent e               = { 0 };
    e.type                 = ClientMessage;
    e.xclient.window       = windowX11(self)->window;
    e.xclient.message_type = XInternAtom(globalX11->display, "_NET_WM_STATE", True);
    e.xclient.format       = 32;
    e.xclient.data.l[0]    = maximized ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE;
    e.xclient.data.l[1]    = XInternAtom(globalX11->display, "_NET_WM_STATE_MAXIMIZED_VERT", True);
    e.xclient.data.l[3]    = 0;

    XSendEvent(globalX11->display,
               DefaultRootWindow(globalX11->display),
               False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               &e);

    e.xclient.data.l[1] = XInternAtom(globalX11->display, "_NET_WM_STATE_MAXIMIZED_HORZ", True);

    XSendEvent(globalX11->display,
               DefaultRootWindow(globalX11->display),
               False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               &e);

    XFlush(globalX11->display);
}

static void WindowX11_resize(struct WindowBase* self, uint32_t w, uint32_t h)
{
    XWindowChanges changes;
    self->w        = w;
    self->h        = h;
    changes.width  = w;
    changes.height = h;

    XConfigureWindow(globalX11->display, windowX11(self)->window, CWWidth, &changes);

    Window_notify_content_change(self);
}

static void WindowX11_events(struct WindowBase* self)
{
    while (XPending(globalX11->display)) {
        XNextEvent(globalX11->display, &windowX11(self)->event);
        XEvent* e = &windowX11(self)->event;
        switch (e->type) {
            case MapNotify:
                FLAG_UNSET(self->state_flags, WINDOW_IS_MINIMIZED);
                Window_notify_content_change(self);
                break;

            case UnmapNotify:
                FLAG_SET(self->state_flags, WINDOW_IS_MINIMIZED);
                break;

            case FocusIn:
                XSetICFocus(globalX11->ic);
                FLAG_SET(self->state_flags, WINDOW_IS_IN_FOCUS);
                self->callbacks.activity_notify_handler(self->callbacks.user_data);
                Window_notify_content_change(self);
                if (Window_is_pointer_hidden(self)) {
                    WindowX11_set_pointer_style(self, MOUSE_POINTER_ARROW);
                }
                break;

            case FocusOut:
                XUnsetICFocus(globalX11->ic);
                if (Window_is_pointer_hidden(self)) {
                    WindowX11_set_pointer_style(self, MOUSE_POINTER_ARROW);
                }
                FLAG_UNSET(self->state_flags, WINDOW_IS_IN_FOCUS);

                break;

            case Expose:
                Window_notify_content_change(self);
                break;

            case ConfigureNotify:
                self->x = e->xconfigure.x;
                self->y = e->xconfigure.y;
                if (self->w != e->xconfigure.width || self->h != e->xconfigure.height) {
                    self->w = e->xconfigure.width;
                    self->h = e->xconfigure.height;
                    Window_notify_content_change(self);
                }
                break;

            case ClientMessage:
                if ((Atom)e->xclient.data.l[0] == globalX11->wm_delete) {
                    FLAG_SET(self->state_flags, WINDOW_IS_CLOSED);
                }
                break;

            case MappingNotify:
                XRefreshKeyboardMapping(&e->xmapping);
                break;

            case KeyPress: {
                Status    stat = 0;
                KeySym    ret;
                char      buf[5] = { 0 };
                uint8_t   bytes  = Xutf8LookupString(globalX11->ic, &e->xkey, buf, 4, &ret, &stat);
                mbstate_t mb     = { 0 };
                uint32_t  code;
                int       no_consume = (stat == 4);
                mbrtoc32(&code, buf, bytes, &mb);

                switch (ret) {
                    case XK_Home:
                    case XK_End:
                    case XK_Right:
                    case XK_Left:
                    case XK_Up:
                    case XK_Down:
                    case XK_Insert:
                    case XK_Delete:
                    case XK_Return:
                    case XK_KP_Enter:
                    case XK_Page_Down:
                    case XK_Page_Up:
                    case XK_KP_Page_Down:
                    case XK_KP_Page_Up:
                    case XK_F1 ... XK_F35:
                    case XK_KP_F1 ... XK_KP_F4:
                        no_consume = 1;
                        break;
                }

                LOG("X::KeyPress{ status:%d, ret:%lu, bytes:%d, code:%u, no_consume:%d }\n",
                    stat,
                    ret,
                    bytes,
                    code,
                    no_consume);

                if (no_consume) {
                    int32_t lower = XkbKeycodeToKeysym(globalX11->display, e->xkey.keycode, 0, 0);
                    CALL_FP(self->callbacks.key_handler,
                            self->callbacks.user_data,
                            stat == 4 ? code : ret,
                            lower,
                            convert_modifier_mask(e->xkey.state));
                }
            } break;

            case ButtonRelease:
                if (e->xbutton.button != 4 && e->xbutton.button != 5 && e->xbutton.button) {
                    CALL_FP(self->callbacks.button_handler,
                            self->callbacks.user_data,
                            e->xbutton.button,
                            false,
                            e->xbutton.x,
                            e->xbutton.y,
                            0,
                            convert_modifier_mask(e->xbutton.state));
                }
                windowX11(self)->last_button_pressed = 0;
                break;

            case ButtonPress: {
                uint32_t btn;
                switch (e->xbutton.button) {
                    case 4:
                        windowX11(self)->last_button_pressed = 0;
                        btn                                  = 65;
                        break;
                    case 5:
                        windowX11(self)->last_button_pressed = 0;
                        btn                                  = 66;
                        break;
                    default:
                        windowX11(self)->last_button_pressed = btn = e->xbutton.button;
                }
                CALL_FP(self->callbacks.button_handler,
                        self->callbacks.user_data,
                        btn,
                        true,
                        e->xbutton.x,
                        e->xbutton.y,
                        0,
                        convert_modifier_mask(e->xbutton.state));
            } break;

            case MotionNotify:
                if (Window_is_pointer_hidden(self)) {
                    WindowX11_set_pointer_style(self, MOUSE_POINTER_ARROW);
                }
                CALL_FP(self->callbacks.motion_handler,
                        self->callbacks.user_data,
                        windowX11(self)->last_button_pressed,
                        e->xmotion.x,
                        e->xmotion.y);
                break;

            case SelectionClear:
                if (windowX11(self)->cliptext) {
                    free((void*)windowX11(self)->cliptext);
                    windowX11(self)->cliptext = NULL;
                }
                break;

            case SelectionRequest:
                if (!windowX11(self)->cliptext) {
                    /* deny */
                    XSelectionEvent se;
                    se.type      = SelectionNotify;
                    se.requestor = e->xselectionrequest.requestor;
                    se.selection = e->xselectionrequest.selection;
                    se.target    = e->xselectionrequest.target;
                    se.property  = None;
                    se.time      = e->xselectionrequest.time;
                    XSendEvent(globalX11->display,
                               e->xselectionrequest.requestor,
                               True,
                               NoEventMask,
                               (XEvent*)&se);
                } else {
                    /* accept */
                    Atom utf8 = XInternAtom(globalX11->display, "UTF8_STRING", 0);

                    XChangeProperty(globalX11->display,
                                    e->xselectionrequest.requestor,
                                    e->xselectionrequest.property,
                                    utf8,
                                    8,
                                    PropModeReplace,
                                    (const unsigned char*)windowX11(self)->cliptext,
                                    strlen(windowX11(self)->cliptext));

                    XSelectionEvent se;
                    se.type      = SelectionNotify;
                    se.requestor = e->xselectionrequest.requestor;
                    se.selection = e->xselectionrequest.selection;
                    se.target    = e->xselectionrequest.target;
                    se.property  = e->xselectionrequest.property;
                    se.time      = e->xselectionrequest.time;
                    XSendEvent(globalX11->display,
                               e->xselectionrequest.requestor,
                               True,
                               NoEventMask,
                               (XEvent*)&se);
                }
                break;

            case SelectionNotify:
                if (e->xselection.property != None) {
                    Atom           clip = XInternAtom(globalX11->display, "CLIPBOARD", 0);
                    Atom           da, type, incr;
                    int            di;
                    unsigned long  dul, size;
                    unsigned char* pret = NULL;
                    incr                = XInternAtom(globalX11->display, "INCR", 0);
                    XGetWindowProperty(globalX11->display,
                                       windowX11(self)->window,
                                       clip,
                                       0,
                                       0,
                                       False,
                                       AnyPropertyType,
                                       &type,
                                       &di,
                                       &dul,
                                       &size,
                                       &pret);
                    if (type != incr) {
                        /* if data is larger than chunk size (200-ish k), it's
                         * probably not text anyway */
                        XGetWindowProperty(globalX11->display,
                                           windowX11(self)->window,
                                           clip,
                                           0,
                                           size,
                                           False,
                                           AnyPropertyType,
                                           &da,
                                           &di,
                                           &dul,
                                           &dul,
                                           &pret);

                        self->callbacks.clipboard_handler(self->callbacks.user_data, (char*)pret);
                        XFree(pret);
                    }
                    XDeleteProperty(globalX11->display, windowX11(self)->window, clip);
                }
                break;

            default:;
        }
    }
}

static void WindowX11_set_current_context(struct WindowBase* self, bool this)
{
    if (this) {
        glXMakeCurrent(globalX11->display, windowX11(self)->window, windowX11(self)->glx_context);
    } else {
        glXMakeCurrent(globalX11->display, None, NULL);
    }
}

static void WindowX11_set_swap_interval(struct WindowBase* self, int32_t ival)
{
    if (glXSwapIntervalEXT) {
        glXSwapIntervalEXT(globalX11->display, windowX11(self)->window, ival);
    }
}

static void WindowX11_set_title(struct WindowBase* self, const char* title)
{
    ASSERT(title, "string is NULL");
    XStoreName(globalX11->display, windowX11(self)->window, title);
    XChangeProperty(globalX11->display,
                    windowX11(self)->window,
                    XInternAtom(globalX11->display, "_NET_WM_NAME", False),
                    XInternAtom(globalX11->display, "UTF8_STRING", False),
                    8,
                    PropModeReplace,
                    (unsigned char*)title,
                    strlen(title));
    XFlush(globalX11->display);
}

static void WindowX11_set_wm_name(struct WindowBase* self,
                                  const char*        class_name,
                                  const char*        opt_name)
{
    ASSERT(class_name, "class name is not NULL");
    XClassHint class_hint = { (char*)class_name, (char*)OR(opt_name, class_name) };
    XSetClassHint(globalX11->display, windowX11(self)->window, &class_hint);
}

static bool WindowX11_maybe_swap(struct WindowBase* self)
{
    if (self->paint && !FLAG_IS_SET(self->state_flags, WINDOW_IS_MINIMIZED)) {
        self->paint = false;

        if (self->callbacks.on_redraw_requested) {
            self->callbacks.on_redraw_requested(self->callbacks.user_data);
        }

        glXSwapBuffers(globalX11->display, windowX11(self)->window);
        return true;
    } else {
        return false;
    }
}

static void WindowX11_destroy(struct WindowBase* self)
{
    XUndefineCursor(globalX11->display, windowX11(self)->window);
    XFreeCursor(globalX11->display, globalX11->cursor_beam);
    XFreeCursor(globalX11->display, globalX11->cursor_hidden);

    XUnmapWindow(globalX11->display, windowX11(self)->window);

    glXMakeCurrent(globalX11->display, 0, 0);
    glXDestroyContext(globalX11->display, windowX11(self)->glx_context);

    XFreeColormap(globalX11->display, windowX11(self)->colormap);

    XDestroyIC(globalX11->ic);
    XCloseIM(globalX11->im);

    XDestroyWindow(globalX11->display, windowX11(self)->window);
    XCloseDisplay(globalX11->display);

    free(self);
}

static int WindowX11_get_connection_fd(struct WindowBase* self)
{
    return ConnectionNumber(globalX11->display);
}

static uint32_t WindowX11_get_keycode_from_name(struct WindowBase* self, char* name)
{
    KeyCode kcode = XStringToKeysym(name);
    return kcode == NoSymbol ? 0 : kcode;
}

static void WindowX11_set_pointer_style(struct WindowBase* self, enum MousePointerStyle style)
{
    switch (style) {
        case MOUSE_POINTER_HIDDEN:
            XDefineCursor(globalX11->display, windowX11(self)->window, globalX11->cursor_hidden);
            FLAG_SET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            break;

        case MOUSE_POINTER_ARROW:
            /* use root window's cursor */
            XUndefineCursor(globalX11->display, windowX11(self)->window);
            FLAG_UNSET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            break;

        case MOUSE_POINTER_I_BEAM:
            XDefineCursor(globalX11->display, windowX11(self)->window, globalX11->cursor_beam);
            FLAG_UNSET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            break;

        case MOUSE_POINTER_HAND:
            XDefineCursor(globalX11->display, windowX11(self)->window, globalX11->cursor_hand);
            FLAG_UNSET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            break;
    }
}

#endif
