// Defines sigaction on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// https://www.x.org/releases/current/doc/libX11/libX11/libX11.html
// TODO: Only define what's necessary.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#define XK_MISCELLANY
#include <X11/keysymdef.h>
#undef XK_MISCELLANY

// https://fedoraproject.org/wiki/Features/XI2#Documentation
#include <X11/extensions/XInput2.h>

// https://www.khronos.org/registry/OpenGL/specs/gl/glx1.4.pdf
#define GL_GLEXT_PROTOTYPES
#include <GL/glx.h>
#undef GL_GLEXT_PROTOTYPES
// #include <GL/glxext.h>

#define STB_IMAGE_IMPLEMENTATION
#include "lib/stb_image.h"

#include "util.h"

#define WINDOW_INIT_W 800
#define WINDOW_INIT_H 600
#define PROGRAM_NAME "i2x"

static const u32 replacement_character_codepoint = 0xFFFD;

static void (*glXSwapIntervalEXT)(Display *dpy, GLXDrawable drawable, int interval);

typedef struct
{
  GLuint texture_id;
  i32 w;
  i32 h;
  u8* pixels;
} img_entry_t;

internal u64 get_nanoseconds()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_nsec + 1000000000 * ts.tv_sec;
}

int main(int argc, char** argv)
{
  Display* display = XOpenDisplay(0);
  if(display)
  {
    int screen_number = 0;
    Window root_window = RootWindow(display, screen_number);

    b32 xi_available = false;
#if 0
    int xi_opcode = 0;
    {
      int query_event = 0;
      int query_error = 0;
      if(XQueryExtension(display, "XInputExtension", &xi_opcode, &query_event, &query_error))
      {
        int major = 2;
        int minor = 0;
        if(XIQueryVersion(display, &major, &minor) == Success)
        {
          xi_available = true;
        }
      }
    }
    if(!xi_available)
    {
      fprintf(stderr, "No XInput2 available.\n");
    }
#endif

    int glx_major = 0;
    int glx_minor = 0;
    if(glXQueryVersion(display, &glx_major, &glx_minor))
    {
      // printf("GLX version: %d.%d\n", glx_major, glx_minor);
      // printf("GLX extensions: %s\n", glXQueryExtensionsString(display, screen_number));

      glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)
        glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");

      int glx_config_attrib_list[] = {
        GLX_DOUBLEBUFFER,   1,
        GLX_RED_SIZE,       8,
        GLX_GREEN_SIZE,     8,
        GLX_BLUE_SIZE,      8,
        // GLX_DEPTH_SIZE,    16,
        // GLX_SAMPLE_BUFFERS, 1,
        // GLX_SAMPLES,        4,
        0
      };
      int glx_config_count = 0;
      GLXFBConfig* glx_configs = glXChooseFBConfig(display, screen_number,
          glx_config_attrib_list, &glx_config_count);
      if(glx_configs && glx_config_count > 0)
      {
        GLXFBConfig glx_config = glx_configs[0];

#if 0
        printf("%d GLX configs available.\n", glx_config_count);
        for(i32 i = 0;
            i < glx_config_count;
            ++i)
        {
          printf("\n%d:\n", i);
          int tmp;
#define PRATTR(x) glXGetFBConfigAttrib(display, glx_configs[i], x, &tmp); printf("  " #x ": %d\n", tmp);
          PRATTR(GLX_DOUBLEBUFFER);
          PRATTR(GLX_RED_SIZE);
          PRATTR(GLX_GREEN_SIZE);
          PRATTR(GLX_BLUE_SIZE);
          PRATTR(GLX_DEPTH_SIZE);
          PRATTR(GLX_SAMPLE_BUFFERS);
          PRATTR(GLX_SAMPLES);
#undef PRATTR
        }
#endif

        XFree(glx_configs);
        GLXContext glx_context = glXCreateNewContext(display, glx_config, GLX_RGBA_TYPE, 0, True);
        if(!glXIsDirect(display, glx_context))
        {
          fprintf(stderr, "GLX context is not direct.  Rendering may be slow.\n");
        }

        XVisualInfo* visual_info = glXGetVisualFromFBConfig(display, glx_config);
        Visual* glx_visual = visual_info->visual;

        XSetWindowAttributes window_attributes = {0};
        window_attributes.event_mask = 0
          | KeyPressMask
          | KeyReleaseMask
          // | EnterWindowMask
          // | LeaveWindowMask
          // | KeymapStateMask
          | StructureNotifyMask
          | FocusChangeMask
          ;
        if(!xi_available)
        {
          window_attributes.event_mask = window_attributes.event_mask
            | ButtonPressMask
            | ButtonReleaseMask
            | PointerMotionMask
            ;
        }
        window_attributes.colormap = XCreateColormap(display, root_window, glx_visual, AllocNone);

        Window window = XCreateWindow(display, root_window,
            0, 0, WINDOW_INIT_W, WINDOW_INIT_H,
            0, visual_info->depth, InputOutput, glx_visual,
            CWEventMask | CWColormap, &window_attributes);

        XFree(visual_info);
        GLXWindow glx_window = glXCreateWindow(display, glx_config, window, 0);

        b32 vsync = true;

        // TODO: Check if GLX_EXT_swap_control is in the extensions string first.
        // Enable VSync.
        glXSwapIntervalEXT(display, glx_window, vsync);

        XChangeProperty(display, window, XA_WM_NAME, XA_STRING, 8, PropModeReplace,
            (unsigned char*)PROGRAM_NAME, sizeof(PROGRAM_NAME) - 1);
        XMapWindow(display, window);

        glXMakeContextCurrent(display, glx_window, glx_window, glx_context);

        Cursor empty_cursor;
        {
          char zero = 0;
          Pixmap empty_pixmap = XCreateBitmapFromData(display, root_window, &zero, 1, 1);
          XColor empty_color = {0};
          empty_cursor = XCreatePixmapCursor(display, empty_pixmap, empty_pixmap,
              &empty_color, &empty_color, 0, 0);
          XFreePixmap(display, empty_pixmap);
        }

        i32 img_count = max(1, argc - 1);
        img_entry_t* img_entries = malloc_array(img_count, img_entry_t);
        zero_bytes(img_count * sizeof(img_entry_t), img_entries);

        if(argc > 1)
        {
          for(i32 img_idx = 0;
              img_idx < img_count;
              ++img_idx)
          {
            img_entry_t* img = &img_entries[img_idx];

            img->pixels = stbi_load(argv[img_idx + 1], &img->w, &img->h, 0, 4);

#if 0
            // Premultiply alpha.
            for(u64 i = 0; i < (u64)img->w * (u64)img->h; ++i)
            {
              if(img->pixels[4*i + 3] != 255)
              {
                r32 r = img->pixels[4*i + 0] / 255.0f;
                r32 g = img->pixels[4*i + 1] / 255.0f;
                r32 b = img->pixels[4*i + 2] / 255.0f;
                r32 a = img->pixels[4*i + 3] / 255.0f;

                img->pixels[4*i + 0] = 255.0f * a * r;
                img->pixels[4*i + 1] = 255.0f * a * g;
                img->pixels[4*i + 2] = 255.0f * a * b;
              }
            }
#endif
          }
        }

        i32 win_w = WINDOW_INIT_W;
        i32 win_h = WINDOW_INIT_H;
        GLuint texture_id = 0;

        r32 time = 0;
        u32 frames_since_last_print = 0;
        u64 nsecs_last_print = get_nanoseconds();
        u64 nsecs_last_frame = nsecs_last_print;
        i64 nsecs_min = I64_MAX;
        i64 nsecs_max = I64_MIN;

        b32 quitting = false;
        i32 viewing_img_idx = 0;
        i32 last_viewing_img_idx = -1;
        b32 border_sampling = true;
        b32 linear_sampling = true;
        b32 alpha_blend = true;
        b32 clear_bg = true;
        b32 srgb = false;
        b32 extra_toggles[10] = {0};
        r32 zoom = 0;
        r32 offset_x = 0;
        r32 offset_y = 0;

        r32 zoom_start_x = 0;
        r32 zoom_start_y = 0;
        r32 prev_mouse_x = 0;
        r32 prev_mouse_y = 0;

        while(!quitting)
        {
          b32 texture_needs_update = false;

          while(XPending(display))
          {
            XEvent event;
            XNextEvent(display, &event);

            switch(event.type)
            {
              case KeyPress:
              // case KeyRelease:
              {
                b32 went_down = (event.type == KeyPress);
                u32 keycode = event.xkey.keycode;
                KeySym keysym = 0;
                keysym = XLookupKeysym(&event.xkey, 0);
                b32 shift_held = (event.xkey.state & 1);
                b32 ctrl_held = (event.xkey.state & 4);

#if 0
                printf("state %#x keycode %u keysym %#lx (%s) %s\n",
                    event.xkey.state, keycode, keysym, XKeysymToString(keysym),
                    went_down ? "Pressed" : "Released");
#endif

                if(keysym == XK_Escape)
                {
                  quitting = true;
                }
                if(keysym == ' ')
                {
                  ++viewing_img_idx;
                  if(viewing_img_idx >= img_count) { viewing_img_idx -= img_count; }
                }
                if(keysym == XK_BackSpace)
                {
                  --viewing_img_idx;
                  if(viewing_img_idx < 0) { viewing_img_idx += img_count; }
                }
                else if(keysym == 'a')
                {
                  bflip(border_sampling);
                }
                else if(keysym == 'b')
                {
                  bflip(clear_bg);
                }
                else if(keysym == 'l')
                {
                  bflip(linear_sampling);
                  texture_needs_update = true;
                }
                else if(keysym == 's')
                {
                  bflip(srgb);

                  if(srgb)
                  {
                    glEnable(GL_FRAMEBUFFER_SRGB);
                  }
                  else
                  {
                    glDisable(GL_FRAMEBUFFER_SRGB);
                  }
                  texture_needs_update = true;
                }
                else if(keysym == 't')
                {
                  bflip(alpha_blend);
                }
                else if(keysym == 'v')
                {
                  bflip(vsync);

                  glXSwapIntervalEXT(display, glx_window, (int)vsync);
                }
                else if(keysym == 'x')
                {
                  zoom = 0;
                  offset_x = 0;
                  offset_y = 0;
                }
                else if(keysym == '0')
                {
                  zoom = 0;
                }
                else if(keysym == '-')
                {
                  zoom -= 0.125f;
                }
                else if(keysym == '=')
                {
                  zoom += 0.125f;
                }
                else if(keysym >= '0' && keysym <= '9')
                {
                  bflip(extra_toggles[keysym - '0']);
                }

                // TODO: Look into XmbLookupString for typed text input.
                // https://www.x.org/releases/current/doc/libX11/libX11/libX11.html#XmbLookupString
              } break;

              case ButtonPress:
              // case ButtonRelease:
              {
                b32 went_down = (event.type == ButtonPress);
                u32 button = event.xbutton.button;
                b32 ctrl_held = (event.xbutton.state & 4);
                b32 alt_held = (event.xbutton.state & 8);
                r32 mouse_x = event.xbutton.x;
                r32 mouse_y = win_h - event.xbutton.y - 1;

#if 0
                printf("state %#x button %u %s\n",
                    event.xbutton.state, button, went_down ? "Pressed" : "Released");
#endif

                r32 win_min_side = min(win_w, win_h);
                r32 exp_zoom_before = exp2f(zoom);
                r32 zoom_per_scroll = 0.125f;
                r32 offset_per_scroll = 0.125f / exp_zoom_before;

                r32 zoom_delta = 0;

                if(0) {}
                else if(button == 4)
                {
                  if(ctrl_held)
                  {
                    zoom_delta = zoom_per_scroll;
                  }
                  else if(alt_held)
                  {
                    --viewing_img_idx;
                    if(viewing_img_idx < 0) { viewing_img_idx += img_count; }
                  }
                  else
                  {
                    offset_y -= offset_per_scroll;
                  }
                }
                else if(button == 5)
                {
                  if(ctrl_held)
                  {
                    zoom_delta = -zoom_per_scroll;
                  }
                  else if(alt_held)
                  {
                    ++viewing_img_idx;
                    if(viewing_img_idx >= img_count) { viewing_img_idx -= img_count; }
                  }
                  else
                  {
                    offset_y += offset_per_scroll;
                  }
                }
                else if(button == 6)
                {
                  if(!ctrl_held)
                  {
                    offset_x += offset_per_scroll;
                  }
                }
                else if(button == 7)
                {
                  if(!ctrl_held)
                  {
                    offset_x -= offset_per_scroll;
                  }
                }

                if(zoom_delta != 0)
                {
                  zoom += zoom_delta;
                  r32 exp_zoom_after = exp2f(zoom);

                  r32 center_mouse_x = mouse_x - 0.5f * win_w;
                  r32 center_mouse_y = mouse_y - 0.5f * win_h;

                  offset_x += center_mouse_x / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
                  offset_y += center_mouse_y / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
                }

                zoom_start_x = mouse_x;
                zoom_start_y = mouse_y;
                prev_mouse_x = mouse_x;
                prev_mouse_y = mouse_y;
              } break;

              case MotionNotify:
              {
                // printf("%x\n", event.xmotion.state);
                b32 ctrl_held = (event.xmotion.state & 4);
                b32 lmb_held = (event.xmotion.state & 0x100);
                b32 mmb_held = (event.xmotion.state & 0x200);

                if(lmb_held || mmb_held)
                {
                  r32 mouse_x = event.xbutton.x;
                  r32 mouse_y = win_h - event.xbutton.y - 1;

                  r32 delta_x = mouse_x - prev_mouse_x;
                  r32 delta_y = mouse_y - prev_mouse_y;

                  r32 win_min_side = min(win_w, win_h);

                  if(ctrl_held || mmb_held)
                  {
                    r32 exp_zoom_before = exp2f(zoom);
                    zoom += 4.0f * delta_y / win_min_side;
                    r32 exp_zoom_after = exp2f(zoom);

                    r32 center_mouse_x = zoom_start_x - 0.5f * win_w;
                    r32 center_mouse_y = zoom_start_y - 0.5f * win_h;

                    offset_x += center_mouse_x / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
                    offset_y += center_mouse_y / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
                  }
                  else
                  {
                    r32 exp_zoom = exp2f(zoom);

                    offset_x += delta_x / (exp_zoom * win_min_side);
                    offset_y += delta_y / (exp_zoom * win_min_side);

                    zoom_start_x = mouse_x;
                    zoom_start_y = mouse_y;
                  }

                  prev_mouse_x = mouse_x;
                  prev_mouse_y = mouse_y;
                }
              } break;

              case FocusIn:
              {
              } break;

              case FocusOut:
              {
              } break;

              case ConfigureNotify:
              {
                win_w = event.xconfigure.width;
                win_h = event.xconfigure.height;
              } break;

              case DestroyNotify:
              {
                quitting = true;
              } break;

              case MappingNotify:
              {
                // printf(stderr, "Key mapping changed\n");
                if(event.xmapping.request == MappingModifier ||
                    event.xmapping.request == MappingKeyboard)
                {
                  XRefreshKeyboardMapping(&event.xmapping);
                }
              } break;

              default:
              {
                // printf("Unhandled event type=%u\n", event.type);
              } break;
            }
          }

          if(last_viewing_img_idx != viewing_img_idx)
          {
            char txt[256];
            txt[0] = 0;
            char* file_name = argv[viewing_img_idx + 1];
            i32 txt_len = snprintf(txt, sizeof(txt), "%s - %s", PROGRAM_NAME, file_name);
            XChangeProperty(display, window, XA_WM_NAME, XA_STRING, 8, PropModeReplace,
                (unsigned char*)txt, txt_len);

            last_viewing_img_idx = viewing_img_idx;
          }

#if 0
          if(game_memory.grab_mouse && !pointer_grabbed)
          {
            XGrabPointer(display, window,
                true, 0, GrabModeAsync, GrabModeAsync, window, None, CurrentTime);
            pointer_grabbed = true;
          }
          else if(!game_memory.grab_mouse && pointer_grabbed)
          {
            XUngrabPointer(display, CurrentTime);
            pointer_grabbed = false;
          }

          if(should_hide_mouse && !cursor_hidden)
          {
            XDefineCursor(display, window, empty_cursor);
            cursor_hidden = true;
          }
          else if(!should_hide_mouse && cursor_hidden)
          {
            XUndefineCursor(display, window);
            cursor_hidden = false;
          }
#endif


          glViewport(0, 0, win_w, win_h);
          if(clear_bg)
          {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
          }
          else
          {
            r32 gray = 0.5f;
            if(srgb)
            {
              gray = powf(gray, 2.2f);
            }
            glClearColor(gray, gray, gray, 1.0f);
          }
          glClear(GL_COLOR_BUFFER_BIT);

          if(win_w != 0 && win_h != 0)
          {
            glMatrixMode(GL_PROJECTION);
            r32 matrix[16] = { // Column-major!
              2.0f / win_w, 0.0f, 0.0f, 0.0f,
              0.0f, 2.0f / win_h, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.0f,
              -1.0f, -1.0f, 0.0f, 1.0f,
            };
            glLoadMatrixf(matrix);
          }

#if 0
          glMatrixMode(GL_MODELVIEW);
          glLoadIdentity();
          glRotatef(30.0f, 0, 0, 1);
#endif

          img_entry_t* img = &img_entries[viewing_img_idx];

          if(!img->texture_id)
          {
            texture_needs_update = true;
            glGenTextures(1, &img->texture_id);
#if 0
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
#endif
          }

          glBindTexture(GL_TEXTURE_2D, img->texture_id);

          u8 test_texels[] = {
            1, 1, 1, 255,
            255, 0, 0, 255,
            0, 255, 0, 255,
            0, 0, 255, 255,
            2, 2, 2, 255,
            255, 255, 0, 255,
            0, 255, 255, 255,
            255, 0, 255, 255,
            128, 128, 128, 255,
          };
          u8* texels = test_texels;
          i32 tex_w = 3;
          i32 tex_h = 3;

          if(img->pixels)
          {
            texels = img->pixels;
            tex_w = img->w;
            tex_h = img->h;
          }

          if(texture_needs_update)
          {
            if(linear_sampling)
            {
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  extra_toggles[1]
                  ? GL_LINEAR
                  : extra_toggles[2] ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
            }
            else
            {
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            }
            glTexImage2D(GL_TEXTURE_2D, 0,
                srgb ? GL_SRGB_ALPHA : GL_RGBA,
                tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
            glGenerateMipmap(GL_TEXTURE_2D);
          }

          if(alpha_blend)
          {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          else
          {
            glDisable(GL_BLEND);
          }

          glEnable(GL_TEXTURE_2D);

          r32 u0 = 0.0f;
          r32 v0 = 0.0f;
          r32 u1 = 1.0f;
          r32 v1 = 1.0f;

          r32 mag = 1.0f;
          if(win_w != 0 && win_h != 0)
          {
            mag = min((r32)win_w / (r32)tex_w, (r32)win_h / (r32)tex_h);
          }
          r32 exp_zoom = exp2f(zoom);
          mag *= exp_zoom;

          r32 x_padding = 0.5f * (win_w - mag * tex_w);
          r32 y_padding = 0.5f * (win_h - mag * tex_h);
          r32 x0 = x_padding;
          r32 y0 = y_padding;
          r32 x1 = x0 + mag * tex_w;
          r32 y1 = y0 + mag * tex_h;

          r32 win_min_side = min(win_w, win_h);
          x0 += win_min_side * exp_zoom * offset_x;
          x1 += win_min_side * exp_zoom * offset_x;
          y0 += win_min_side * exp_zoom * offset_y;
          y1 += win_min_side * exp_zoom * offset_y;

          if(border_sampling)
          {
            r32 margin = max(1.0f, mag);

            u0 -= margin / (r32)(mag * tex_w);
            v0 -= margin / (r32)(mag * tex_h);
            u1 += margin / (r32)(mag * tex_w);
            v1 += margin / (r32)(mag * tex_h);

            x0 -= margin;
            y0 -= margin;
            x1 += margin;
            y1 += margin;
          }

          glBegin(GL_QUADS);
          glTexCoord2f(u0, v0); glVertex2f(x0, y1);
          glTexCoord2f(u1, v0); glVertex2f(x1, y1);
          glTexCoord2f(u1, v1); glVertex2f(x1, y0);
          glTexCoord2f(u0, v1); glVertex2f(x0, y0);
          glEnd();

          glXSwapBuffers(display, glx_window);

          if(vsync)
          {
            // This seems to reduce lag on HP laptop.
            // TODO: Test on other computers as well.
            glFinish();
          }

          u64 nsecs_now = get_nanoseconds();
          i64 nsecs = nsecs_now - nsecs_last_frame;
          nsecs_last_frame = nsecs_now;
#if 1
          if(!vsync)
          {
            ++frames_since_last_print;
            nsecs_min = min(nsecs_min, nsecs);
            nsecs_max = max(nsecs_max, nsecs);
            r32 secs_since_last_print = 1e-9f * (r32)(nsecs_now - nsecs_last_print);
            if(secs_since_last_print >= 0.5f)
            {
              printf("avg FPS: %.1f [%.1f - %.1f]\n",
                  (r32)frames_since_last_print / secs_since_last_print,
                  1e9f / (r32)nsecs_max,
                  1e9f / (r32)nsecs_min);
              frames_since_last_print = 0;
              nsecs_last_print = nsecs_now;
              nsecs_min = I64_MAX;
              nsecs_max = I64_MIN;
            }
          }
#endif

          // time += 1.0f / 60.0f;
          time += 1e-9f * nsecs;
          if(time >= 1000) { time -= 1000; }
        }
      }
      else
      {
        fprintf(stderr, "No GLX configs available.\n");
      }
    }
    else
    {
      fprintf(stderr, "Could not query GLX version.\n");
    }
  }
  else
  {
    fprintf(stderr, "Could not open X11 display.\n");
  }
}
