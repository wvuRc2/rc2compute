# rc2compute

This is the R backend portion of the rc2 project. It builds `rserver` which listens for connection requests and then forks a copy of `rsession`. Each rsession process wraps an R session via RInside.

All communication is via json. Files are managed via the PostgreSQL database. Configuration is via [etcd](). See the [overview wiki page](https://github.com/wvuRc2/rc2/wiki) for details on how etcd is configured.

rserver takes a command line argument for which type of deployment to use and what srv record to look up. The default srv record is `config.rc2.io`. Once connected to the server the srv record defines, a connection is made. The key path is by another parameter, defaulting to `dev`. [cetcd](https://github.com/shafreeck/cetcd.git) is used to connect to etcd. It be installed in /usr/local.
