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

#include "util.h"

#if 0
#define WINDOW_INIT_W 800
#define WINDOW_INIT_H 600
#else
#define WINDOW_INIT_W 20
#define WINDOW_INIT_H 20
#endif
#define WINDOW_TITLE "i2x"

static const u32 replacement_character_codepoint = 0xFFFD;

static void (*glXSwapIntervalEXT)(Display *dpy, GLXDrawable drawable, int interval);

typedef struct
{
} ui_state_t;

internal u32 decode_utf8(u8** start, u8* end)
{
  u8 c = **start;
  u32 codepoint = 0;
  i32 extra_byte_count = 0;
  if(c & 0x80)
  {
    if((c & 0xE0) == 0xC0)
    {
      codepoint = (c & 0x1F);
      extra_byte_count = 1;
    }
    else if((c & 0xF0) == 0xE0)
    {
      codepoint = (c & 0x0F);
      extra_byte_count = 2;
    }
    else if((c & 0xF8) == 0xF0)
    {
      codepoint = (c & 0x07);
      extra_byte_count = 3;
    }
    else
    {
      // Invalid non-continuation byte.
      codepoint = replacement_character_codepoint;
    }

    if(*start + extra_byte_count < end)
    {
      b32 invalid_continuation = false;
      for(i32 extra_byte_idx = 0;
          extra_byte_idx < extra_byte_count;
          ++extra_byte_idx)
      {
        ++*start;
        c = **start;
        invalid_continuation = !is_utf8_continuation_byte(c);
        if(invalid_continuation)
        {
          // Expected continuation byte, but didn't get it.
          --*start;
          codepoint = replacement_character_codepoint;
          break;
        }
        codepoint = (codepoint << 6) | (c & 0x3F);
      }
    }
    else
    {
      // Expected more bytes.
      codepoint = replacement_character_codepoint;
    }
  }
  else
  {
    // ASCII.
    codepoint = (u32)c;
  }
  ++*start;

  return codepoint;
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
        // GLX_SAMPLE_BUFFERS, 0,
        // GLX_SAMPLES,        0, // 4,
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

        // TODO: Check if GLX_EXT_swap_control is in the extensions string first.
        // Enable VSync.
        glXSwapIntervalEXT(display, glx_window, 1);

        XChangeProperty(display, window, XA_WM_NAME, XA_STRING, 8, PropModeReplace,
            (unsigned char*)WINDOW_TITLE, sizeof(WINDOW_TITLE) - 1);
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

        i32 win_w = WINDOW_INIT_W;
        i32 win_h = WINDOW_INIT_H;
        GLuint texture_id = 0;
        r32 time = 0;
        b32 quitting = false;
        b32 border_sampling = true;
        b32 linear_sampling = true;
        b32 alpha_blend = true;
        b32 extra_magnification = false;
        b32 clear_bg = true;
        b32 srgb_framebuffer = false;
        b32 srgb_texture = false;
        b32 extra_toggles[10] = {0};

        while(!quitting)
        {
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
                b32 shift_held = (event.xkey.state & 0x01);

                printf("state %#x keycode %u keysym %#lx (%s) %s\n",
                    event.xkey.state, keycode, keysym, XKeysymToString(keysym),
                    went_down ? "Pressed" : "Released");

                if(keysym == XK_Escape)
                {
                  quitting = true;
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
                }
                else if(keysym == 'm')
                {
                  bflip(extra_magnification);
                }
                else if(keysym == 's')
                {
                  bflip(srgb_framebuffer);
                }
                else if(keysym == 'd')
                {
                  bflip(srgb_texture);
                }
                else if(keysym == 't')
                {
                  bflip(alpha_blend);
                }
                else if(keysym >= '0' && keysym <= '9')
                {
                  bflip(extra_toggles[keysym - '0']);
                }

                // TODO: Look into XmbLookupString for typed text input.
                // https://www.x.org/releases/current/doc/libX11/libX11/libX11.html#XmbLookupString
              } break;

              case ButtonPress:
              case ButtonRelease:
              {
                b32 went_down = (event.type == ButtonPress);
                u32 button = event.xbutton.button;

                printf("state %#x button %u %s\n",
                    event.xbutton.state, button, went_down ? "Pressed" : "Released");

                if(button == 1)
                {
                }
                else if(button == 4)
                {
                  // game_input.mouse_scroll.y += 1;
                }
                else if(button == 5)
                {
                  // game_input.mouse_scroll.y -= 1;
                }
                else if(button == 6)
                {
                  // game_input.mouse_scroll.x -= 1;
                }
                else if(button == 7)
                {
                  // game_input.mouse_scroll.x += 1;
                }
              } break;

              case MotionNotify:
              {
                // game_input.mouse_pos.x = (r32)event.xmotion.x;
                // game_input.mouse_pos.y = render_context.frame_size.y - (r32)event.xmotion.y - 1.0f;
              } break;

              case FocusIn:
              {
                // has_focus = true;
              } break;

              case FocusOut:
              {
#if 0
                for(u32 button_idx = 0;
                    button_idx < array_count(game_input.buttons);
                    ++button_idx)
                {
                  game_input.buttons[button_idx].ended_down = 0;
                }
                game_input.modifiers_held = 0;
                has_focus = false;
#endif
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
                // write_stderr("Key mapping changed\n");
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

          if(srgb_framebuffer)
          {
            glEnable(GL_FRAMEBUFFER_SRGB);
          }
          else
          {
            glDisable(GL_FRAMEBUFFER_SRGB);
          }

          glViewport(0, 0, win_w, win_h);
          if(clear_bg)
          {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
          }
          else
          {
            r32 gray = 0.5f;
            if(srgb_framebuffer)
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

          u8 texels[] = {
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
          i32 tex_w = 3;
          i32 tex_h = 3;

          if(!texture_id)
          {
            glGenTextures(1, &texture_id);
            glBindTexture(GL_TEXTURE_2D, texture_id);
#if 0
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
#endif
            // glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
            // glGenerateMipmap(GL_TEXTURE_2D);
          }

          glTexImage2D(GL_TEXTURE_2D, 0, srgb_texture ? GL_SRGB_ALPHA : GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
          glGenerateMipmap(GL_TEXTURE_2D);

          glEnable(GL_TEXTURE_2D);

          if(linear_sampling)
          {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

#if 0
            // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
#else
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                extra_toggles[1]
                ? GL_LINEAR
                : extra_toggles[2] ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
#endif
          }
          else
          {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
          }

          if(alpha_blend)
          {
            glEnable(GL_BLEND);
            // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          else
          {
            glDisable(GL_BLEND);
          }

          r32 u0 = 0.0f;
          r32 v0 = 0.0f;
          r32 u1 = 1.0f;
          r32 v1 = 1.0f;

          r32 mag = 4.0f;
          if(extra_magnification) { mag *= 32; }

          r32 x0 = 2 + 1.0f * fmodf(time, 50);
          r32 x1 = x0 + mag * tex_w;
          r32 y0 = 2 + 0.1f * fmodf(time, 50);
          r32 y1 = y0 + mag * tex_h;

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
          glTexCoord2f(u0, v0); glVertex2f(x0, y0);
          glTexCoord2f(u1, v0); glVertex2f(x1, y0);
          glTexCoord2f(u1, v1); glVertex2f(x1, y1);
          glTexCoord2f(u0, v1); glVertex2f(x0, y1);
          glEnd();

          glXSwapBuffers(display, glx_window);
          if(!extra_toggles[0])
          {
            time += 1.0f / 60.0f;
            if(time >= 1000) { time -= 1000; }
          }
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
