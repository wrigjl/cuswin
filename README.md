# cuswin
Windows cu for named pipes.

This program is used to connect to the virtual serial ports provided by VirtualBox, Hyper-V, etc.
Primarily, I use it to connect to the serial console of virtual machines for kernel debugged.

## Usage

cus [-l log] named-pipe
cus -i named-pipe

For example, suppose I have a virtual machine with the first UART set to be a pipe called "foo".
To connect to it:

```
cus \\.\pipe\foo
```

To exit, type [return]~. (return, tilde, dot).

Cus can optionally keep a log of output read from the serial port using the -l switch. If you want the same program
roughly for UNIX, try [cus](https://github.com/wrigjl/cus).

In the second usage (-i), cus will print permission infomation about the pipe, e.g.

```
C:\Users\rigel\source\repos\cus\Release>cus -i \\.\pipe\test
owner: NT VIRTUAL MACHINE\228356C1-78E3-4854-BE1B-A0FEE8548EF8
group: NT VIRTUAL MACHINE\Virtual Machines
dacl: revision 2 count 5 bytes-used 136 bytes-free 0
0: type 0 flags=0 access-allowed mask=120089(read,readea,readattr,readcontrol,sync) sid: \Everyone
1: type 0 flags=0 access-allowed mask=1f01ff(read,write,createpipe,readea,writeea,execute,delete-child,readattr,writeattr,delete,readcontrol,writedac,writeowner,sync) sid: BUILTIN\Hyper-V Administrators
2: type 0 flags=0 access-allowed mask=1f01ff(read,write,createpipe,readea,writeea,execute,delete-child,readattr,writeattr,delete,readcontrol,writedac,writeowner,sync) sid: BUILTIN\Administrators
3: type 0 flags=0 access-allowed mask=1f01ff(read,write,createpipe,readea,writeea,execute,delete-child,readattr,writeattr,delete,readcontrol,writedac,writeowner,sync) sid: NT AUTHORITY\SYSTEM
4: type 0 flags=0 access-allowed mask=1f01ff(read,write,createpipe,readea,writeea,execute,delete-child,readattr,writeattr,delete,readcontrol,writedac,writeowner,sync) sid: NT VIRTUAL MACHINE\228356C1-78E3-4854-BE1B-A0FEE8548EF8
```

This shows a pipe for a virtual machine. Everyone can get the status of the pipe, but only Administrators and "Hyper-V Administrators" can actually use it.
