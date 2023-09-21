# lcdfs
File system interface for Hitachi HD44780 LCD paird with a PCF8574 for I2C

Check the default configuration in threadmain() to see if it matches what your system does.
if not, they can be changed at the command line
lcdfs -s (/Srv name) -m(mount point) -d(i2c device file)
