# A plugin that holds a write to the tarball cache still in one phase, for
# `tarball-cancel.sh`. A static Nix cannot load plugins; the test then skips
# the cases that need it.
tarball_cache_pause_plugin = []
nix_fetchers = dependency('nix-fetchers', required : false)
have_tarball_cache_pause_plugin = (
  nix_fetchers.found()
  and get_option('default_library') != 'static'
)
if have_tarball_cache_pause_plugin
  tarball_cache_pause_plugin = shared_module(
    'tarball-cache-pause',
    'plugin.cc',
    dependencies : [ nix_fetchers ],
    build_by_default : false,
  )
endif
