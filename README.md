Jalv
====

Jalv is a simple but fully featured LV2 host for Jack which exposes plugin
ports to Jack, essentially making any LV2 plugin function as a Jack
application.
For more information, see <http://drobilla.net/software/jalv>.

Jalv is a small program which is useful, but also intended to be an appropriate
test host for plugin development.  It runs plugins from the command line with
no user interaction, is light enough to run in valgrind, and is capable of
dumping all communication between the plugin and its UI in a human readable
format.

To make it simple to test plugins in hosts that use different toolkits, there
are several Jalv executables: jalv, jalv.gtk, jalv.gtk3, jalv.gtkmm, jalv.qt4,
and jalv.qt5.

 -- David Robillard <d@drobilla.net>
