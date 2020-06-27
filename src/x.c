/* See LICENSE for license information. */

#ifndef NOX // if x11 support enabled at compile time

#define _GNU_SOURCE

#include "x.h"

#include <uchar.h>

#include <GL/glx.h>
#include <X11/X.h>
#include <X11/XKBlib.h>
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

typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*,
                                                     GLXFBConfig,
                                                     GLXContext,
                                                     Bool,
                                                     const int*);

typedef void (*glXSwapIntervalARBProc)(Display*    dpy,
                                       GLXDrawable drawable,
                                       int         interval);

glXSwapIntervalARBProc glXSwapIntervalEXT = NULL;

static WindowStatic* global;

#define globalX11       ((GlobalX11*)&global->subclass_data)
#define windowX11(base) ((WindowX11*)&base->extend_data)

struct WindowBase* WindowX11_new(uint32_t w, uint32_t h);

void WindowX11_set_fullscreen(struct WindowBase* self, bool fullscreen);

void WindowX11_resize(struct WindowBase* self, uint32_t w, uint32_t h);

void WindowX11_events(struct WindowBase* self);

void WindowX11_set_wm_name(struct WindowBase* self, const char* title);

void WindowX11_set_title(struct WindowBase* self, const char* title);

void WindowX11_set_swap_interval(struct WindowBase* self, int32_t ival);

void WindowX11_maybe_swap(struct WindowBase* self);

void WindowX11_destroy(struct WindowBase* self);

int WindowX11_get_connection_fd(struct WindowBase* self);

void WindowX11_clipboard_get(struct WindowBase* self);

void WindowX11_clipboard_send(struct WindowBase* self, const char* text);

void* WindowX11_get_gl_ext_proc_adress(struct WindowBase* self,
                                       const char*        name);

uint32_t WindowX11_get_keycode_from_name(void* self, char* name);

static struct IWindow window_interface_x11 = {
    .set_fullscreen         = WindowX11_set_fullscreen,
    .resize                 = WindowX11_resize,
    .events                 = WindowX11_events,
    .set_title              = WindowX11_set_title,
    .set_app_id             = WindowX11_set_wm_name,
    .maybe_swap             = WindowX11_maybe_swap,
    .destroy                = WindowX11_destroy,
    .get_connection_fd      = WindowX11_get_connection_fd,
    .clipboard_send         = WindowX11_clipboard_send,
    .clipboard_get          = WindowX11_clipboard_get,
    .set_swap_interval      = WindowX11_set_swap_interval,
    .get_gl_ext_proc_adress = WindowX11_get_gl_ext_proc_adress,
    .get_keycode_from_name  = WindowX11_get_keycode_from_name,
};

typedef struct
{
    Display*     display;
    XVisualInfo* visual_info;
    Atom         wm_delete;

    Cursor cursor_hidden;

    XIM im;
    XIC ic;

    XIMStyles* im_styles;
    XIMStyle   im_style;

} GlobalX11;

typedef struct
{
    Window               window;
    GLXContext           glx_context;
    XEvent               event;
    XSetWindowAttributes set_win_attribs;
    Colormap             colormap;
    uint32_t             last_button_pressed;
    uint32_t             mods;
    const char*          cliptext;
    XClassHint*          class_hint;
} WindowX11;

void WindowX11_clipboard_send(struct WindowBase* self, const char* text)
{
    if (windowX11(self)->cliptext)
        free((void*)windowX11(self)->cliptext);

    Atom sel = XInternAtom(globalX11->display, "CLIPBOARD", False);
    windowX11(self)->cliptext = text;
    XSetSelectionOwner(globalX11->display, sel, windowX11(self)->window,
                       CurrentTime);
}

void WindowX11_clipboard_get(struct WindowBase* self)
{
    Atom   clip  = XInternAtom(globalX11->display, "CLIPBOARD", 0);
    Atom   utf8  = XInternAtom(globalX11->display, "UTF8_STRING", 0);
    Window owner = XGetSelectionOwner(globalX11->display, clip);

    if (owner != None)
        XConvertSelection(globalX11->display, clip, utf8, clip,
                          windowX11(self)->window, CurrentTime);
}

static void WindowX11_setup_pointer(struct WindowBase* self)
{
    XColor      c       = { .red = 0, .green = 0, .blue = 0 };
    static char data[8] = { 0 };

    Pixmap pmp = XCreateBitmapFromData(globalX11->display,
                                       windowX11(self)->window, data, 8, 8);

    globalX11->cursor_hidden =
      XCreatePixmapCursor(globalX11->display, pmp, pmp, &c, &c, 0, 0);
}

static void WindowX11_pointer(struct WindowBase* self, bool hide)
{
    if (hide && !FLAG_IS_SET(self->state_flags, WINDOW_POINTER_HIDDEN)) {

        XDefineCursor(globalX11->display, windowX11(self)->window,
                      globalX11->cursor_hidden);

        FLAG_SET(self->state_flags, WINDOW_POINTER_HIDDEN);

    } else if (!hide && FLAG_IS_SET(self->state_flags, WINDOW_POINTER_HIDDEN)) {

        XUndefineCursor(globalX11->display, windowX11(self)->window);

        FLAG_UNSET(self->state_flags, WINDOW_POINTER_HIDDEN);
    }
}

void* WindowX11_get_gl_ext_proc_adress(struct WindowBase* self,
                                       const char*        name)
{
    return glXGetProcAddress((const GLubyte*)name);
}

struct WindowBase* WindowX11_new(uint32_t w, uint32_t h)
{
    global =
      calloc(1, sizeof(WindowStatic) + sizeof(GlobalX11) - sizeof(uint8_t));

    globalX11->display = XOpenDisplay(NULL);

    if (!globalX11->display) {
        free(global);
        return NULL;
    }

    int glx_major, glx_minor, qry_res;
    if (!(qry_res =
            glXQueryVersion(globalX11->display, &glx_major, &glx_minor)) ||
        (glx_major == 1 && glx_minor < 3)) {
        WRN("GLX version to low\n");
        free(global);
        return NULL;
    }

    if (!XSupportsLocale())
        ERR("Xorg does not support locales\n");

    struct WindowBase* win = calloc(1, sizeof(struct WindowBase) +
                                         sizeof(WindowX11) - sizeof(uint8_t));

    XSetLocaleModifiers("@im=none");

    globalX11->im = XOpenIM(globalX11->display, NULL, NULL, NULL);

    globalX11->ic = XCreateIC(globalX11->im, XNInputStyle,
                              XIMPreeditNothing | XIMStatusNothing,
                              XNClientWindow, windowX11(win)->window, NULL);

    if (!globalX11->ic)
        ERR("Failed to create IC\n");

    XSetICFocus(globalX11->ic);

    win->w         = w;
    win->h         = h;
    win->interface = &window_interface_x11;

    static const int visual_attribs[] = { GLX_RENDER_TYPE,
                                          GLX_RGBA_BIT,
                                          GLX_DRAWABLE_TYPE,
                                          GLX_WINDOW_BIT,
                                          GLX_DOUBLEBUFFER,
                                          True,
                                          GLX_RED_SIZE,
                                          8,
                                          GLX_GREEN_SIZE,
                                          8,
                                          GLX_BLUE_SIZE,
                                          8,
                                          GLX_ALPHA_SIZE,
                                          8,
                                          GLX_DEPTH_SIZE,
                                          16,
                                          None };

    int          fb_cfg_cnt;
    GLXFBConfig* fb_cfg =
      glXChooseFBConfig(globalX11->display, DefaultScreen(globalX11->display),
                        visual_attribs, &fb_cfg_cnt);

    int fb_cfg_sel = 0;
    for (fb_cfg_sel = 0; fb_cfg_sel < fb_cfg_cnt; ++fb_cfg_sel) {
        globalX11->visual_info =
          glXGetVisualFromFBConfig(globalX11->display, fb_cfg[fb_cfg_sel]);

        if (!globalX11->visual_info)
            continue;

        XRenderPictFormat* pf = XRenderFindVisualFormat(
          globalX11->display, globalX11->visual_info->visual);

        if (pf->direct.alphaMask > 0)
            break;

        XFree(globalX11->visual_info);
    }

    if (!globalX11->visual_info)
        ERR("Failed to get X11 visual info");

    windowX11(win)->set_win_attribs = (XSetWindowAttributes){
        .colormap = windowX11(win)->colormap = XCreateColormap(
          globalX11->display,
          RootWindow(globalX11->display, globalX11->visual_info->screen),
          globalX11->visual_info->visual, AllocNone),
        .border_pixel      = 0,
        .background_pixmap = None,
        .override_redirect = True,
        .event_mask        = KeyPressMask | KeyReleaseMask | ButtonPressMask |
                      ButtonReleaseMask | SubstructureRedirectMask |
                      StructureNotifyMask | PointerMotionMask | ExposureMask |
                      FocusChangeMask | KeymapStateMask | VisibilityChangeMask
    };

    windowX11(win)->glx_context = NULL;

    static int context_attribs[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
                                     GLX_CONTEXT_MINOR_VERSION_ARB, 1, None };

    glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 0;

    glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddressARB(
      (const GLubyte*)"glXSwapIntervalEXT");

    glXCreateContextAttribsARB =
      (glXCreateContextAttribsARBProc)glXGetProcAddress(
        (const GLubyte*)"glXCreateContextAttribsARB");

    windowX11(win)->glx_context = glXCreateContextAttribsARB(
      globalX11->display, fb_cfg[fb_cfg_sel], 0, True, context_attribs);

    if (!windowX11(win)->glx_context)
        ERR("Failed to create GLX context");

    windowX11(win)->window = XCreateWindow(
      globalX11->display,
      RootWindow(globalX11->display, globalX11->visual_info->screen), 0, 0,
      win->w, win->h, 0, globalX11->visual_info->depth, InputOutput,
      globalX11->visual_info->visual, CWBorderPixel | CWColormap | CWEventMask,
      &windowX11(win)->set_win_attribs);

    if (!windowX11(win)->window)
        ERR("Failed to create X11 window");

    XFree(fb_cfg);
    XFree(globalX11->visual_info);

    XMapWindow(globalX11->display, windowX11(win)->window);

    glXMakeCurrent(globalX11->display, windowX11(win)->window,
                   windowX11(win)->glx_context);

    XSync(globalX11->display, False);

    globalX11->wm_delete =
      XInternAtom(globalX11->display, "WM_DELETE_WINDOW", True);

    XSetWMProtocols(globalX11->display, windowX11(win)->window,
                    &globalX11->wm_delete, 1);

    XFlush(globalX11->display);

    WindowX11_setup_pointer(win);

    /* get refresh rate from xrandr */
    XRRScreenConfiguration* xrr_s_conf =
      XRRGetScreenInfo(globalX11->display, RootWindow(globalX11->display, 0));

    short xrr_ref_rate = xrr_s_conf ? XRRConfigCurrentRate(xrr_s_conf) : 0;

    LOG("Detected refresh rate: %d fps\n", xrr_ref_rate);
    if (xrr_ref_rate > 1) {
        global->target_frame_time_ms = 1000 / xrr_ref_rate;
    } else {
        global->target_frame_time_ms = 1000 / 60;
    }

    XRRFreeScreenConfigInfo(xrr_s_conf);

    XkbSelectEvents(globalX11->display, XkbUseCoreKbd, XkbAllEventsMask,
                    XkbAllEventsMask);

    windowX11(win)->class_hint            = XAllocClassHint();
    windowX11(win)->class_hint->res_class = strdup(settings.title);
    XSetClassHint(globalX11->display, windowX11(win)->window,
                  windowX11(win)->class_hint);

    return win;
}

struct WindowBase* Window_new_x11(Pair_uint32_t res)
{
    struct WindowBase* win = WindowX11_new(res.first, res.second);

    if (!win)
        return NULL;

    win->title = NULL;
    WindowX11_set_title(win, settings.title);
    WindowX11_set_wm_name(win, settings.title);

    return win;
}

static inline void WindowX11_fullscreen_change_state(struct WindowBase* self,
                                                     const long         arg)
{

    Atom wm_state = XInternAtom(globalX11->display, "_NET_WM_STATE", True);
    Atom wm_fullscreen =
      XInternAtom(globalX11->display, "_NET_WM_STATE_FULLSCREEN", True);

    XClientMessageEvent e = { .type         = ClientMessage,
                              .window       = windowX11(self)->window,
                              .message_type = wm_state,
                              .format       = 32,
                              .data = { { (long)arg, wm_fullscreen, 0 } } };

    XSendEvent(globalX11->display, DefaultRootWindow(globalX11->display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, (XEvent*)&e);
}

void WindowX11_set_fullscreen(struct WindowBase* self, bool fullscreen)
{
    if (fullscreen && !FLAG_IS_SET(self->state_flags, WINDOW_FULLSCREEN)) {
        WindowX11_fullscreen_change_state(self, _NET_WM_STATE_ADD);
        FLAG_SET(self->state_flags, WINDOW_FULLSCREEN);
    } else if (!fullscreen &&
               FLAG_IS_SET(self->state_flags, WINDOW_FULLSCREEN)) {
        WindowX11_fullscreen_change_state(self, _NET_WM_STATE_REMOVE);
        FLAG_UNSET(self->state_flags, WINDOW_FULLSCREEN);
    }
}

void WindowX11_resize(struct WindowBase* self, uint32_t w, uint32_t h)
{
    XWindowChanges changes;
    self->w        = w;
    self->h        = h;
    changes.width  = w;
    changes.height = h;

    XConfigureWindow(globalX11->display, windowX11(self)->window, CWWidth,
                     &changes);

    Window_notify_content_change(self);
}

void WindowX11_events(struct WindowBase* self)
{
    while (XPending(globalX11->display)) {
        XNextEvent(globalX11->display, &windowX11(self)->event);

        XEvent* e = &windowX11(self)->event;

        switch (e->type) {
            case MapNotify:
                Window_notify_content_change(self);
                break;

            case FocusIn:
                FLAG_SET(self->state_flags, WINDOW_IN_FOCUS);
                self->callbacks.activity_notify_handler(
                  self->callbacks.user_data);
                Window_notify_content_change(self);
                break;

            case FocusOut:
                if (FLAG_IS_SET(self->state_flags, WINDOW_POINTER_HIDDEN))
                    WindowX11_pointer(self, false);

                FLAG_UNSET(self->state_flags, WINDOW_IN_FOCUS);
                break;

            case Expose:
                Window_notify_content_change(self);
                break;

            case ConfigureNotify:
                self->x = e->xconfigure.x;
                self->y = e->xconfigure.y;

                if (self->w != e->xconfigure.width ||
                    self->h != e->xconfigure.height) {
                    self->w = e->xconfigure.width;
                    self->h = e->xconfigure.height;
                    Window_notify_content_change(self);
                }
                break;

            case ClientMessage:
                if ((Atom)e->xclient.data.l[0] == globalX11->wm_delete)
                    FLAG_SET(self->state_flags, WINDOW_CLOSED);
                break;

            case MappingNotify:
                XRefreshKeyboardMapping(&e->xmapping);
                break;

            case KeyPress:;
                Status  stat = 0;
                KeySym  ret;
                char    buf[5] = { 0 };
                uint8_t bytes  = Xutf8LookupString(globalX11->ic, &e->xkey, buf,
                                                  4, &ret, &stat);
                mbstate_t mb   = { 0 };
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
                    case XK_Return:
                    case XK_KP_Enter:
                        no_consume = 1;
                        break;

                    case XK_Control_L:
                    case XK_Control_R:
                        FLAG_SET(windowX11(self)->mods, MODIFIER_CONTROL);
                        break;
                    case XK_Shift_L:
                    case XK_Shift_R:
                        FLAG_SET(windowX11(self)->mods, MODIFIER_SHIFT);
                        break;
                    case XK_Alt_L:
                    case XK_Alt_R:
                        FLAG_SET(windowX11(self)->mods, MODIFIER_ALT);
                        break;

                    default:
                        if (ret >= XK_F1 && ret <= XK_F24)
                            no_consume = 1;
                }

                if (no_consume) {
                    int32_t always_lower = XkbKeycodeToKeysym(
                      globalX11->display, e->xkey.keycode, 0, 0);
                    self->callbacks.key_handler(
                      self->callbacks.user_data, stat == 4 ? code : ret,
                      always_lower, windowX11(self)->mods);
                }

                WindowX11_pointer(self, true);
                break;

            case KeyRelease:
                WindowX11_pointer(self, true);

                switch (XLookupKeysym(&e->xkey, 0)) {
                    case XK_Control_L:
                    case XK_Control_R:
                        FLAG_UNSET(windowX11(self)->mods, MODIFIER_CONTROL);
                        break;
                    case XK_Shift_L:
                    case XK_Shift_R:
                        FLAG_UNSET(windowX11(self)->mods, MODIFIER_SHIFT);
                        break;
                    case XK_Alt_L:
                    case XK_Alt_R:
                        FLAG_UNSET(windowX11(self)->mods, MODIFIER_ALT);
                        break;
                    default:;
                }
                break;

            case ButtonRelease:
                if (e->xbutton.button != 4 && e->xbutton.button) {
                    self->callbacks.button_handler(
                      self->callbacks.user_data, e->xbutton.button, false,
                      e->xbutton.x, e->xbutton.y, 0, 0);
                }
                windowX11(self)->last_button_pressed = 0;
                WindowX11_pointer(self, false);
                break;

            case ButtonPress:
                if (e->xbutton.button == 4) {
                    windowX11(self)->last_button_pressed = 0;
                    self->callbacks.button_handler(
                      self->callbacks.user_data, 65, true, e->xbutton.x,
                      e->xbutton.y, 0, windowX11(self)->mods);
                } else if (e->xbutton.button == 5) {
                    windowX11(self)->last_button_pressed = 0;
                    self->callbacks.button_handler(
                      self->callbacks.user_data, 66, true, e->xbutton.x,
                      e->xbutton.y, 0, windowX11(self)->mods);
                } else {
                    windowX11(self)->last_button_pressed = e->xbutton.button;
                    self->callbacks.button_handler(
                      self->callbacks.user_data, e->xbutton.button, true,
                      e->xbutton.x, e->xbutton.y, 0, windowX11(self)->mods);
                }
                WindowX11_pointer(self, false);
                break;

            case MotionNotify:
                if (windowX11(self)->last_button_pressed) {
                    self->callbacks.motion_handler(
                      self->callbacks.user_data,
                      windowX11(self)->last_button_pressed, e->xmotion.x,
                      e->xmotion.y);
                }
                WindowX11_pointer(self, false);
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
                               e->xselectionrequest.requestor, True,
                               NoEventMask, (XEvent*)&se);
                } else {
                    /* accept */
                    Atom utf8 =
                      XInternAtom(globalX11->display, "UTF8_STRING", 0);

                    XChangeProperty(
                      globalX11->display, e->xselectionrequest.requestor,
                      e->xselectionrequest.property, utf8, 8, PropModeReplace,
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
                               e->xselectionrequest.requestor, True,
                               NoEventMask, (XEvent*)&se);
                }
                break;

            case SelectionNotify:
                if (e->xselection.property != None) {
                    Atom clip = XInternAtom(globalX11->display, "CLIPBOARD", 0);
                    Atom da, type, incr;
                    int  di;
                    unsigned long  dul, size;
                    unsigned char* pret = NULL;
                    incr = XInternAtom(globalX11->display, "INCR", 0);
                    XGetWindowProperty(
                      globalX11->display, windowX11(self)->window, clip, 0, 0,
                      False, AnyPropertyType, &type, &di, &dul, &size, &pret);
                    if (type != incr) {
                        /* if data is larger than chunk size (200-ish k), it's
                         * probably not text anyway */
                        XGetWindowProperty(globalX11->display,
                                           windowX11(self)->window, clip, 0,
                                           size, False, AnyPropertyType, &da,
                                           &di, &dul, &dul, &pret);

                        self->callbacks.clipboard_handler(
                          self->callbacks.user_data, (char*)pret);
                        XFree(pret);
                    }
                    XDeleteProperty(globalX11->display, windowX11(self)->window,
                                    clip);
                }
                break;

            default:;
        }
    }
}

void WindowX11_set_current_context(struct WindowBase* self) {}

void WindowX11_set_swap_interval(struct WindowBase* self, int32_t ival)
{
    if (glXSwapIntervalEXT) {
        glXSwapIntervalEXT(globalX11->display, windowX11(self)->window, ival);
    } else {
        WRN("glXSwapIntervalEXT not found\n");
    }
}

void WindowX11_set_title(struct WindowBase* self, const char* title)
{
    ASSERT(title, "string is NULL");

    XStoreName(globalX11->display, windowX11(self)->window, title);
}

void WindowX11_set_wm_name(struct WindowBase* self, const char* title)
{
    ASSERT(title, "string is NULL");

    XSetIconName(globalX11->display, windowX11(self)->window, title);
}

void WindowX11_maybe_swap(struct WindowBase* self)
{
    if (self->paint) {
        self->paint = false;
        glXSwapBuffers(globalX11->display, windowX11(self)->window);
    } else {
        usleep(1000 * (FLAG_IS_SET(self->state_flags, WINDOW_IN_FOCUS)
                         ? global->target_frame_time_ms - 3
                         : global->target_frame_time_ms));
    }
}

void WindowX11_destroy(struct WindowBase* self)
{
    XUnmapWindow(globalX11->display, windowX11(self)->window);

    glXMakeCurrent(globalX11->display, 0, 0);
    glXDestroyContext(globalX11->display, windowX11(self)->glx_context);

    XFreeColormap(globalX11->display, windowX11(self)->colormap);

    XDestroyIC(globalX11->ic);
    XCloseIM(globalX11->im);

    free(windowX11(self)->class_hint->res_class);
    XFree(windowX11(self)->class_hint);

    XDestroyWindow(globalX11->display, windowX11(self)->window);
    XCloseDisplay(globalX11->display);

    free(self);
}

int WindowX11_get_connection_fd(struct WindowBase* self)
{
    return ConnectionNumber(globalX11->display);
}

uint32_t WindowX11_get_keycode_from_name(void* self, char* name)
{
    KeyCode kcode = XStringToKeysym(name);
    return kcode == NoSymbol ? 0 : kcode;
}

#endif
