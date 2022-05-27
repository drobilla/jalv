Jalv
====

Jalv (JAck LV2) is a simple host for LV2 plugins.  It runs a plugin, and
exposes the plugin ports to the system, essentially making the plugin an
application.  For more information, see <http://drobilla.net/software/jalv>.

Jalv can be built to run on JACK, where plugin ports are exposed directly as
JACK ports, or via PortAudio, where the plugin is connected to the system
inputs and outputs.

Jalv is particularly useful as a simple test host for plugin development and
testing.  It runs plugins from the command line with no user interaction, is
light enough to run with tools like sanitizers or valgrind, and is capable of
dumping all communication between the plugin and its UI in a human readable
format.  Plugin UIs can be tested in different host toolkits by using different
executables: jalv, jalv.gtk, jalv.gtk3, jalv.gtkmm, and jalv.qt5.

 -- David Robillard <d@drobilla.net>
