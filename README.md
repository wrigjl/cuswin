# cuswin
Windows cu for named pipes.

This program is used to connect to the virtual serial ports provided by VirtualBox, Hyper-V, etc.
Primarily, I use it to connect to the serial console of virtual machines for kernel debugged.

## Usage

cus [-l log] named-pipe

For example, suppose I have a virtual machine with the first UART set to be a pipe called "foo".
To connect to it:

```
cus \\.\pipe\foo
```

To exit, type [return]~. (return, tilde, dot).

Cus can optionally keep a log of output read from the serial port using the -l switch. If you want the same program
roughly for UNIX, try [cus](https://github.com/wrigjl/cus).
