// -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 2; -*-

#ifdef HAVE_CONFIG_H
# include "../config.h"
#endif

extern "C" {
#ifdef    HAVE_STDIO_H
#  include <stdio.h>
#endif // HAVE_STDIO_H

#ifdef    HAVE_UNISTD_H
#  include <sys/types.h>
#  include <unistd.h>
#endif // HAVE_UNISTD_H

#include "gettext.h"
#define _(str) gettext(str)
}

#include "screen.hh"
#include "client.hh"
#include "openbox.hh"
#include "frame.hh"
#include "bindings.hh"
#include "python.hh"
#include "otk/display.hh"

#include <vector>
#include <algorithm>

static bool running;
static int anotherWMRunning(Display *display, XErrorEvent *) {
  printf(_("Another window manager already running on display %s.\n"),
         DisplayString(display));
  running = true;
  return -1;
}


namespace ob {


OBScreen::OBScreen(int screen)
  : _number(screen),
    _root(screen)
{
  assert(screen >= 0); assert(screen < ScreenCount(otk::OBDisplay::display));
  _info = otk::OBDisplay::screenInfo(screen);

  ::running = false;
  XErrorHandler old = XSetErrorHandler(::anotherWMRunning);
  XSelectInput(otk::OBDisplay::display, _info->rootWindow(),
               OBScreen::event_mask);
  XSync(otk::OBDisplay::display, false);
  XSetErrorHandler(old);

  _managed = !::running;
  if (! _managed) return; // was unable to manage the screen

  printf(_("Managing screen %d: visual 0x%lx, depth %d\n"),
         _number, XVisualIDFromVisual(_info->visual()), _info->depth());

  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::openbox_pid,
                                     otk::OBProperty::Atom_Cardinal,
                                     (unsigned long) getpid());

  // set the mouse cursor for the root window (the default cursor)
  XDefineCursor(otk::OBDisplay::display, _info->rootWindow(),
                Openbox::instance->cursors().session);

  // initialize the shit that is used for all drawing on the screen
  _image_control = new otk::BImageControl(Openbox::instance->timerManager(),
                                          _info, true);
  _image_control->installRootColormap();
  _root_cmap_installed = True;

  // initialize the screen's style
  _style.setImageControl(_image_control);
  std::string stylepath;
  python_get_string("theme", &stylepath);
  otk::Configuration sconfig(false);
  sconfig.setFile(otk::expandTilde(stylepath));
  if (!sconfig.load()) {
    sconfig.setFile(otk::expandTilde(DEFAULTSTYLE));
    if (!sconfig.load()) {
      printf(_("Unable to load default style: %s. Aborting.\n"), DEFAULTSTYLE);
      ::exit(1);
    }
  }
  _style.load(sconfig);

  // set up notification of netwm support
  setSupportedAtoms();

  // Set the netwm properties for geometry and viewport
  unsigned long geometry[] = { _info->width(),
                               _info->height() };
  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::net_desktop_geometry,
                                     otk::OBProperty::Atom_Cardinal,
                                     geometry, 2);
  unsigned long viewport[] = { 0, 0 };
  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::net_desktop_viewport,
                                     otk::OBProperty::Atom_Cardinal,
                                     viewport, 2);

  // Set the net_desktop_names property
  std::vector<std::string> names;
  python_get_stringlist("desktop_names", &names);
  _root.setDesktopNames(names);
  
  // create the window which gets focus when no clients get it
  XSetWindowAttributes attr;
  attr.override_redirect = true;
  _focuswindow = XCreateWindow(otk::OBDisplay::display, _info->rootWindow(),
                               -100, -100, 1, 1, 0, 0, InputOnly,
                               _info->visual(), CWOverrideRedirect, &attr);
  XMapWindow(otk::OBDisplay::display, _focuswindow);
  
  // these may be further updated if any pre-existing windows are found in
  // the manageExising() function
  setClientList();     // initialize the client lists, which will be empty
  calcArea();          // initialize the available working area
}


OBScreen::~OBScreen()
{
  if (! _managed) return;

  XSelectInput(otk::OBDisplay::display, _info->rootWindow(), NoEventMask);
  
  // unmanage all windows
  while (!clients.empty())
    unmanageWindow(clients.front());

  XDestroyWindow(otk::OBDisplay::display, _focuswindow);
  XDestroyWindow(otk::OBDisplay::display, _supportwindow);

  delete _image_control;
}


void OBScreen::manageExisting()
{
  unsigned int i, j, nchild;
  Window r, p, *children;
  XQueryTree(otk::OBDisplay::display, _info->rootWindow(), &r, &p,
             &children, &nchild);

  // preen the window list of all icon windows... for better dockapp support
  for (i = 0; i < nchild; i++) {
    if (children[i] == None) continue;

    XWMHints *wmhints = XGetWMHints(otk::OBDisplay::display,
                                    children[i]);

    if (wmhints) {
      if ((wmhints->flags & IconWindowHint) &&
          (wmhints->icon_window != children[i])) {
        for (j = 0; j < nchild; j++) {
          if (children[j] == wmhints->icon_window) {
            children[j] = None;
            break;
          }
        }
      }

      XFree(wmhints);
    }
  }

  // manage shown windows
  for (i = 0; i < nchild; ++i) {
    if (children[i] == None)
      continue;

    XWindowAttributes attrib;
    if (XGetWindowAttributes(otk::OBDisplay::display, children[i], &attrib)) {
      if (attrib.override_redirect) continue;

      if (attrib.map_state != IsUnmapped) {
        manageWindow(children[i]);
      }
    }
  }

  XFree(children);
}


void OBScreen::updateStrut()
{
  _strut.left = _strut.right = _strut.top = _strut.bottom = 0;

  OBClient::List::iterator it, end = clients.end();
  for (it = clients.begin(); it != end; ++it) {
    const otk::Strut &s = (*it)->strut();
    _strut.left = std::max(_strut.left, s.left);
    _strut.right = std::max(_strut.right, s.right);
    _strut.top = std::max(_strut.top, s.top);
    _strut.bottom = std::max(_strut.bottom, s.bottom);
  }
  calcArea();
}


void OBScreen::calcArea()
{
  otk::Rect old_area = _area;

/*
#ifdef    XINERAMA
  // reset to the full areas
  if (isXineramaActive())
    xineramaUsableArea = getXineramaAreas();
#endif // XINERAMA
*/
  
  _area.setRect(_strut.left, _strut.top,
                _info->width() - (_strut.left + _strut.right),
                _info->height() - (_strut.top + _strut.bottom));

/*
#ifdef    XINERAMA
  if (isXineramaActive()) {
    // keep each of the ximerama-defined areas inside the strut
    RectList::iterator xit, xend = xineramaUsableArea.end();
    for (xit = xineramaUsableArea.begin(); xit != xend; ++xit) {
      if (xit->x() < usableArea.x()) {
        xit->setX(usableArea.x());
        xit->setWidth(xit->width() - usableArea.x());
      }
      if (xit->y() < usableArea.y()) {
        xit->setY(usableArea.y());
        xit->setHeight(xit->height() - usableArea.y());
      }
      if (xit->x() + xit->width() > usableArea.width())
        xit->setWidth(usableArea.width() - xit->x());
      if (xit->y() + xit->height() > usableArea.height())
        xit->setHeight(usableArea.height() - xit->y());
    }
  }
#endif // XINERAMA
*/
  
  if (old_area != _area)
    // XXX: re-maximize windows

  setWorkArea();
}


void OBScreen::setSupportedAtoms()
{
  // create the netwm support window
  _supportwindow = XCreateSimpleWindow(otk::OBDisplay::display,
                                       _info->rootWindow(),
                                       0, 0, 1, 1, 0, 0, 0);
  assert(_supportwindow != None);

  // set supporting window
  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::net_supporting_wm_check,
                                     otk::OBProperty::Atom_Window,
                                     _supportwindow);

  //set properties on the supporting window
  Openbox::instance->property()->set(_supportwindow,
                                     otk::OBProperty::net_wm_name,
                                     otk::OBProperty::utf8,
                                     "Openbox");
  Openbox::instance->property()->set(_supportwindow,
                                     otk::OBProperty::net_supporting_wm_check,
                                     otk::OBProperty::Atom_Window,
                                     _supportwindow);

  
  Atom supported[] = {
/*
      otk::OBProperty::net_current_desktop,
      otk::OBProperty::net_number_of_desktops,
*/
      otk::OBProperty::net_desktop_geometry,
      otk::OBProperty::net_desktop_viewport,
      otk::OBProperty::net_active_window,
      otk::OBProperty::net_workarea,
      otk::OBProperty::net_client_list,
      otk::OBProperty::net_client_list_stacking,
      otk::OBProperty::net_desktop_names,
      otk::OBProperty::net_close_window,
      otk::OBProperty::net_wm_name,
      otk::OBProperty::net_wm_visible_name,
      otk::OBProperty::net_wm_icon_name,
      otk::OBProperty::net_wm_visible_icon_name,
/*
      otk::OBProperty::net_wm_desktop,
*/
      otk::OBProperty::net_wm_strut,
      otk::OBProperty::net_wm_window_type,
      otk::OBProperty::net_wm_window_type_desktop,
      otk::OBProperty::net_wm_window_type_dock,
      otk::OBProperty::net_wm_window_type_toolbar,
      otk::OBProperty::net_wm_window_type_menu,
      otk::OBProperty::net_wm_window_type_utility,
      otk::OBProperty::net_wm_window_type_splash,
      otk::OBProperty::net_wm_window_type_dialog,
      otk::OBProperty::net_wm_window_type_normal,
/*
      otk::OBProperty::net_wm_moveresize,
      otk::OBProperty::net_wm_moveresize_size_topleft,
      otk::OBProperty::net_wm_moveresize_size_topright,
      otk::OBProperty::net_wm_moveresize_size_bottomleft,
      otk::OBProperty::net_wm_moveresize_size_bottomright,
      otk::OBProperty::net_wm_moveresize_move,
*/
/*
      otk::OBProperty::net_wm_allowed_actions,
      otk::OBProperty::net_wm_action_move,
      otk::OBProperty::net_wm_action_resize,
      otk::OBProperty::net_wm_action_shade,
      otk::OBProperty::net_wm_action_maximize_horz,
      otk::OBProperty::net_wm_action_maximize_vert,
      otk::OBProperty::net_wm_action_change_desktop,
      otk::OBProperty::net_wm_action_close,
*/
      otk::OBProperty::net_wm_state,
      otk::OBProperty::net_wm_state_modal,
      otk::OBProperty::net_wm_state_maximized_vert,
      otk::OBProperty::net_wm_state_maximized_horz,
      otk::OBProperty::net_wm_state_shaded,
      otk::OBProperty::net_wm_state_skip_taskbar,
      otk::OBProperty::net_wm_state_skip_pager,
      otk::OBProperty::net_wm_state_hidden,
      otk::OBProperty::net_wm_state_fullscreen,
      otk::OBProperty::net_wm_state_above,
      otk::OBProperty::net_wm_state_below,
    };
  const int num_supported = sizeof(supported)/sizeof(Atom);

  // convert to the atom values
  for (int i = 0; i < num_supported; ++i)
    supported[i] =
      Openbox::instance->property()->atom((otk::OBProperty::Atoms)supported[i]);
  
  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::net_supported,
                                     otk::OBProperty::Atom_Atom,
                                     supported, num_supported);
}


void OBScreen::setClientList()
{
  Window *windows;
  unsigned int size = clients.size();

  // create an array of the window ids
  if (size > 0) {
    Window *win_it;
    
    windows = new Window[size];
    win_it = windows;
    OBClient::List::const_iterator it = clients.begin();
    const OBClient::List::const_iterator end = clients.end();
    for (; it != end; ++it, ++win_it)
      *win_it = (*it)->window();
  } else
    windows = (Window*) 0;

  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::net_client_list,
                                     otk::OBProperty::Atom_Window,
                                     windows, size);

  if (size)
    delete [] windows;

  setStackingList();
}


void OBScreen::setStackingList()
{
  Window *windows;
  unsigned int size = _stacking.size();

  assert(size == clients.size()); // just making sure.. :)

  
  // create an array of the window ids
  if (size > 0) {
    Window *win_it;
    
    windows = new Window[size];
    win_it = windows;
    OBClient::List::const_iterator it = _stacking.begin();
    const OBClient::List::const_iterator end = _stacking.end();
    for (; it != end; ++it, ++win_it)
      *win_it = (*it)->window();
  } else
    windows = (Window*) 0;

  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::net_client_list_stacking,
                                     otk::OBProperty::Atom_Window,
                                     windows, size);

  if (size)
    delete [] windows;
}


void OBScreen::setWorkArea() {
  unsigned long area[] = { _area.x(), _area.y(),
                           _area.width(), _area.height() };
  Openbox::instance->property()->set(_info->rootWindow(),
                                     otk::OBProperty::net_workarea,
                                     otk::OBProperty::Atom_Cardinal,
                                     area, 4);
  /*
  if (workspacesList.size() > 0) {
    unsigned long *dims = new unsigned long[4 * workspacesList.size()];
    for (unsigned int i = 0, m = workspacesList.size(); i < m; ++i) {
      // XXX: this could be different for each workspace
      const otk::Rect &area = availableArea();
      dims[(i * 4) + 0] = area.x();
      dims[(i * 4) + 1] = area.y();
      dims[(i * 4) + 2] = area.width();
      dims[(i * 4) + 3] = area.height();
    }
    xatom->set(getRootWindow(), otk::OBProperty::net_workarea,
               otk::OBProperty::Atom_Cardinal,
               dims, 4 * workspacesList.size());
    delete [] dims;
  } else
    xatom->set(getRootWindow(), otk::OBProperty::net_workarea,
               otk::OBProperty::Atom_Cardinal, 0, 0);
  */
}


void OBScreen::manageWindow(Window window)
{
  OBClient *client = 0;
  XWMHints *wmhint;
  XSetWindowAttributes attrib_set;

  // is the window a docking app
  if ((wmhint = XGetWMHints(otk::OBDisplay::display, window))) {
    if ((wmhint->flags & StateHint) &&
        wmhint->initial_state == WithdrawnState) {
      //slit->addClient(w); // XXX: make dock apps work!
      XFree(wmhint);
      return;
    }
    XFree(wmhint);
  }

  otk::OBDisplay::grab();

  // choose the events we want to receive on the CLIENT window
  attrib_set.event_mask = OBClient::event_mask;
  attrib_set.do_not_propagate_mask = OBClient::no_propagate_mask;
  XChangeWindowAttributes(otk::OBDisplay::display, window,
                          CWEventMask|CWDontPropagate, &attrib_set);

  // create the OBClient class, which gets all of the hints on the window
  client = new OBClient(_number, window);
  // register for events
  Openbox::instance->registerHandler(window, client);
  // add to the wm's map
  Openbox::instance->addClient(window, client);

  // we dont want a border on the client
  client->toggleClientBorder(false);
  
  // specify that if we exit, the window should not be destroyed and should be
  // reparented back to root automatically
  XChangeSaveSet(otk::OBDisplay::display, window, SetModeInsert);

  if (!client->positionRequested()) {
    // XXX: position the window intelligenty
  }

  // create the decoration frame for the client window
  client->frame = new OBFrame(client, &_style);

  // add to the wm's map
  Openbox::instance->addClient(client->frame->window(), client);
  Openbox::instance->addClient(client->frame->plate(), client);
  Openbox::instance->addClient(client->frame->titlebar(), client);
  Openbox::instance->addClient(client->frame->label(), client);
  Openbox::instance->addClient(client->frame->button_max(), client);
  Openbox::instance->addClient(client->frame->button_iconify(), client);
  Openbox::instance->addClient(client->frame->button_stick(), client);
  Openbox::instance->addClient(client->frame->button_close(), client);
  Openbox::instance->addClient(client->frame->handle(), client);
  Openbox::instance->addClient(client->frame->grip_left(), client);
  Openbox::instance->addClient(client->frame->grip_right(), client);

  // XXX: if on the current desktop..
  client->frame->show();
 
  // XXX: handle any requested states such as maximized

  otk::OBDisplay::ungrab();

  // add to the screen's list
  clients.push_back(client);
  // this puts into the stacking order, then raises it
  _stacking.push_back(client);
  restack(true, client);
  // update the root properties
  setClientList();

  Openbox::instance->bindings()->grabButtons(true, client);

  // XXX: make this optional or more intelligent
  if (client->normal())
    client->focus();

  // call the python NEWWINDOW binding
  EventData *data = new_event_data(window, EventNewWindow, 0);
  Openbox::instance->bindings()->fireEvent(data);
  Py_DECREF((PyObject*)data);

  printf("Managed window 0x%lx\n", window);
}


void OBScreen::unmanageWindow(OBClient *client)
{
  OBFrame *frame = client->frame;

  // call the python CLOSEWINDOW binding 
  EventData *data = new_event_data(client->window(), EventCloseWindow, 0);
  Openbox::instance->bindings()->fireEvent(data);
  Py_DECREF((PyObject*)data);

  Openbox::instance->bindings()->grabButtons(false, client);

  // remove from the stacking order
  _stacking.remove(client);
  
  // pass around focus if this window was focused XXX do this better!
  if (Openbox::instance->focusedClient() == client) {
    OBClient *newfocus = 0;
    OBClient::List::iterator it, end = _stacking.end();
    for (it = _stacking.begin(); it != end; ++it)
      if ((*it)->normal() && (*it)->focus()) {
        newfocus = *it;
        break;
      }
    if (!newfocus)
      client->unfocus();
  }

  // remove from the wm's map
  Openbox::instance->removeClient(client->window());
  Openbox::instance->removeClient(frame->window());
  Openbox::instance->removeClient(frame->plate());
  Openbox::instance->removeClient(frame->titlebar());
  Openbox::instance->removeClient(frame->label());
  Openbox::instance->removeClient(frame->button_max());
  Openbox::instance->removeClient(frame->button_iconify());
  Openbox::instance->removeClient(frame->button_stick());
  Openbox::instance->removeClient(frame->button_close());
  Openbox::instance->removeClient(frame->handle());
  Openbox::instance->removeClient(frame->grip_left());
  Openbox::instance->removeClient(frame->grip_right());
  // unregister for handling events
  Openbox::instance->clearHandler(client->window());
  
  // remove the window from our save set
  XChangeSaveSet(otk::OBDisplay::display, client->window(), SetModeDelete);

  // we dont want events no more
  XSelectInput(otk::OBDisplay::display, client->window(), NoEventMask);

  frame->hide();

  // give the client its border back
  client->toggleClientBorder(true);

  delete client->frame;
  client->frame = 0;

  // remove from the screen's list
  clients.remove(client);
  delete client;

  // update the root properties
  setClientList();
}

void OBScreen::restack(bool raise, OBClient *client)
{
  const int layer = client->layer();
  std::vector<Window> wins;

  _stacking.remove(client);

  // the stacking list is from highest to lowest
  
  OBClient::List::iterator it = _stacking.begin(), end = _stacking.end();
  // insert the windows above this window
  for (; it != end; ++it) {
    if ((*it)->layer() < layer || (raise && (*it)->layer() == layer))
      break;
    wins.push_back((*it)->frame->window());
  }
  // insert our client
  wins.push_back(client->frame->window());
  _stacking.insert(it, client);
  // insert the remaining below this window
  for (; it != end; ++it)
    wins.push_back((*it)->frame->window());

  XRestackWindows(otk::OBDisplay::display, &wins[0], wins.size());
  setStackingList();
}

}
