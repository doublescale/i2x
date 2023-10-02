// Defines sigaction on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <inttypes.h>
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
#include <GL/glx.h>
#include <GL/glxext.h>
#include <GL/gl.h>

#include "util.h"

#define WINDOW_INIT_W 800
#define WINDOW_INIT_H 600
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
        GLX_SAMPLE_BUFFERS, 1,
        GLX_SAMPLES,        1, // 4,
        0
      };
      int glx_config_count = 0;
      GLXFBConfig* glx_configs = glXChooseFBConfig(display, screen_number,
          glx_config_attrib_list, &glx_config_count);
      if(glx_configs && glx_config_count > 0)
      {
        // printf("%d GLX configs available.\n", glx_config_count);

        GLXFBConfig glx_config = glx_configs[0];
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
        b32 quitting = false;

        while(!quitting)
        {
          while(XPending(display))
          {
            XEvent event;
            XNextEvent(display, &event);

            switch(event.type)
            {
              case KeyPress:
              case KeyRelease:
              {
                b32 went_down = (event.type == KeyPress);
                u32 keycode = event.xkey.keycode;
                KeySym keysym = 0;
                keysym = XLookupKeysym(&event.xkey, 0);

                printf("state %#x keycode %u keysym %#lx (%s) %s\n",
                    event.xkey.state, keycode, keysym, XKeysymToString(keysym),
                    went_down ? "Pressed" : "Released");

                if(keysym == XK_Escape)
                {
                  quitting = true;
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

          glViewport(0, 0, win_w, win_h);
          glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
          glClear(GL_COLOR_BUFFER_BIT);

          glBegin(GL_TRIANGLES);
          glVertex2f(-0.5f, -0.5f);
          glVertex2f( 0.5f, -0.5f);
          glVertex2f( 0.0f,  0.5f);
          glEnd();

          glXSwapBuffers(display, glx_window);
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
