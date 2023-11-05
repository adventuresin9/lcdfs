
/*
 *	A filsystem interface for a I²C LCD
 *	module sold by GeeekPi.
 *	A Hitachi HD44780 LCD with an
 *	added PCF8574 for I²C comms.
 */

#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>


/* commands */
#define	LCD_CLEARDISPLAY	0x01
#define	LCD_RETURNHOME		0x02
#define	LCD_ENTRYMODESET	0x04
#define	LCD_DISPLAYCONTROL	0x08
#define	LCD_CURSORSHIFT		0x10
#define	LCD_FUNCTIONSET		0x20
#define	LCD_SETCGRAMADDR	0x40
#define	LCD_SETDDRAMADDR	0x80

/* flags for display entry mode LCD_ENTRYMODESET */
#define	LCD_ENTRYRIGHT		0x00
#define	LCD_ENTRYLEFT		0x02
#define	LCD_ENTRYSHIFTINC	0x01
#define	LCD_ENTRYSHIFTDEC	0x00

/* flags for display on/off control LCD_DISPLAYCONTROL */
#define	LCD_DISPLAYON		0x04
#define	LCD_DISPLAYOFF		0x00
#define	LCD_CURSORON		0x02
#define	LCD_CURSOROFF		0x00
#define	LCD_BLINKON			0x01
#define	LCD_BLINKOFF		0x00

/* flags for display/cursor shift LCD_CURSORSHIFT */
#define	LCD_DISPLAYMOVE		0x08
#define	LCD_CURSORMOVE		0x00
#define	LCD_MOVERIGHT		0x04
#define	LCD_MOVELEFT		0x00

/* flags for function set LCD_FUNCTIONSET */
#define	LCD_8BITMODE		0x10
#define	LCD_4BITMODE		0x00
#define	LCD_2LINE			0x08
#define	LCD_1LINE			0x00
#define	LCD_5x10DOTS		0x04
#define	LCD_5x8DOTS			0x00

/* flags for backlight control */
#define	LCD_BACKLIGHT		0x08
#define	LCD_NOBACKLIGHT		0x00

#define	EN		0x04
#define	RW		0x02
#define	RS		0x01


typedef struct Devfile Devfile;
typedef struct LCDdev LCDdev;

static void	ropen(Req *r);
static void	rread(Req *r);
static void	rwrite(Req *r);
static void	initfs(char *dirname);
static int	initchip(void);
static char*	writechar(Req *r);
static char*	readctl(Req *r);
static char*	writectl(Req *r);



struct Devfile {
	char	*name;
	char*	(*doread)(Req*);
	char*	(*dowrite)(Req*);
	int	mode;
};


Devfile files[] = {
	{ "ctl", readctl, writectl, 0664 },
	{ "row1", nil, writechar, 0220 },
	{ "row2", nil, writechar, 0220 },
	{ "row3", nil, writechar, 0220 },
	{ "row4", nil, writechar, 0220 },
};


enum {
	CTLlight,
	CTLdisplay,
	CTLclear,
};


static Cmdtab lcdctlmsg[] =
{
	CTLlight,	"backlight",	2,
	CTLdisplay,	"display",		2,
	CTLclear,	"clear",		2,
};


Srv s = {
	.open = ropen,
	.read = rread,
	.write = rwrite,
};


struct LCDdev {
	u8int	light;
	u8int	entry;
	u8int	display;
	u8int	shift;
	u8int	function;
	u8int	row;
};


File *root;
File *devdir;
LCDdev lcd;
int i²cfd;
int	rows, cols;

static void
ropen(Req *r)
{
	respond(r, nil);
}


static void
rread(Req *r)
{
	Devfile *f;

	r->ofcall.count = 0;
	f = r->fid->file->aux;
	respond(r, f->doread(r));
}


static void
rwrite(Req *r)
{
	Devfile *f;

	if(r->ifcall.count == 0){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}

	f = r->fid->file->aux;
	respond(r, f->dowrite(r));
}


/* this is for sending I²C data just to the PCF8574 */
static void
lcdi2c(u8int cmd)
{
	uchar out[1];
	out[0] = (uchar)cmd;
	pwrite(i²cfd, out, 1, 0);
	sleep(1);
}


/* the HD44780 requires 4-bit data to be sent twice */
static void
lcdwr(u8int cmd)
{
	uchar out[1];

	out[0] = (uchar)(cmd | EN );
	pwrite(i²cfd, out, 1, 0);
	sleep(1);

	out[0] = (uchar)(cmd & ~EN);
	pwrite(i²cfd, out, 1, 0);
	sleep(1);
}


/* the backlight bit need to be sent with every command */
static void
lcdcmd(u8int val)
{
	u8int cmd1, cmd2, mode;

	mode = lcd.light;

	cmd1 = (mode | (val & 0xF0));
	cmd2 = (mode | ((val << 4) & 0xF0));

	lcdwr(cmd1);
	lcdwr(cmd2);
}


/* character data needs to be sent with the RS bit set */
static void
lcdchar(u8int val)
{
	u8int cmd1, cmd2, mode;

	mode = lcd.light | RS;

	cmd1 = (mode | (val & 0xF0));
	cmd2 = (mode | ((val << 4) & 0xF0));

	lcdwr(cmd1);
	lcdwr(cmd2);
}


/*
 *	since the backlight is controled 
 *	by the PCF8574, just a straight I²C 
 *	write is done to it to change it
 */
static void
lcdlight(int state)
{
	if(state > 0)
		lcd.light = LCD_BACKLIGHT;	
	else
		lcd.light = LCD_NOBACKLIGHT;

	lcdi2c(lcd.light);
}


static void
lcddisplay(int state)
{
	if(state > 0)
		lcd.display |= LCD_DISPLAYON;
	else
		lcd.display &= ~LCD_DISPLAYON;

	lcdcmd(lcd.display);
}


static void
lcdclear(void)
{
	lcdcmd(LCD_CLEARDISPLAY);
	lcdcmd(LCD_RETURNHOME);
}


static void
lcdhome(void)
{
// row0 0x00
// row1 0x40
// row2 0x14
// row3 0x54
	u8int rowval[] = {0x00, 0x40, 0x14, 0x54};

	lcdcmd(LCD_SETDDRAMADDR | (rowval[lcd.row]));
}


static void
lcdconfig(void)
{
	lcdcmd(lcd.function);
	lcdcmd(lcd.display);
	lcdcmd(LCD_CLEARDISPLAY);
	lcdcmd(lcd.shift);
	lcdcmd(lcd.entry);
	sleep(5);
}


static void
lcdinit(void)
{
	lcd.light = LCD_BACKLIGHT;
	lcd.function = LCD_FUNCTIONSET | LCD_2LINE | LCD_5x8DOTS | LCD_4BITMODE;
	lcd.display = LCD_DISPLAYCONTROL | LCD_DISPLAYON;
	lcd.shift = LCD_CURSORSHIFT;
	lcd.entry = LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDEC;
	lcd.row = 0;

/* other examples show spamming cleardisplay for init */

	lcdcmd(LCD_CLEARDISPLAY | LCD_RETURNHOME);
	lcdcmd(LCD_CLEARDISPLAY | LCD_RETURNHOME);
	lcdcmd(LCD_CLEARDISPLAY | LCD_RETURNHOME);
	lcdcmd(LCD_RETURNHOME);

	lcdconfig();
}


static void
lcdcrash(char *err)
{
	close(i²cfd);
	threadexitsall(err);
}


static char*
writechar(Req *r)
{
	int len, out, i;
	u8int val;
	u8int space = 0x20;
	char *this = r->fid->file->name;
	char *row2 = "row2";

	len = r->ifcall.count;
	out = 0;
		
	if(len == 0){
		r->ofcall.count = 0;
		return nil;
	}

/* only bother printing chars of ammount cols (columns) */
	if(len > cols)
		len = cols;

	if(!strcmp(this, "row1"))
		lcd.row = 0;

	if(!strcmp(this, "row2"))
		lcd.row = 1;

	if(!strcmp(this, "row3"))
		lcd.row = 2;

	if(!strcmp(this, "row4"))
		lcd.row = 3;

	lcdhome();

	for(i = 0; i < 16; i++){
		if(i < len)
			val = (u8int)r->ifcall.data[i];
		if((val < 0x20) || (val > 0x7E) || (i >= len))		/* do space for all other characters */
			val = space;
		lcdchar(val);
		out++;
	}

	r->ofcall.count = out;
	if(out < r->ifcall.count)		/* return the full count rather than error */
		r->ofcall.count = r->ifcall.count;
	return nil;
}


static char*
readctl(Req *r)
{
	char buf[1024], *p;
	int light, display;

	memset(buf, 0, 1024);

	if(lcd.light == LCD_BACKLIGHT)
		light = 1;
	else
		light = 0;

	if((lcd.display & LCD_DISPLAYON) == LCD_DISPLAYON)
		display = 1;
	else
		display = 0;

	p = buf;
	p = seprint(p, buf + sizeof(buf), "backlight %d\n", light);
	p = seprint(p, buf + sizeof(buf), "display %d\n", display);
	p = seprint(p, buf + sizeof(buf), "clear 0\n");

	readstr(r, buf);
	return nil;
}


static char*
writectl(Req *r)
{
	Cmdbuf *cb;
	Cmdtab *ct;
	long buf;

	cb = parsecmd(r->ifcall.data, r->ifcall.count);
	ct = lookupcmd(cb, lcdctlmsg, nelem(lcdctlmsg));
	buf = strtol(cb->f[1], nil, 0);

	switch(ct->index){
	case CTLlight:
		lcdlight(buf);
		break;
	case CTLdisplay:
		lcddisplay(buf);
		break;
	case CTLclear:
		if(buf > 0)
			lcdclear();
		break;
	default:
		free(cb);
		return "I don't understand";
	}

	r->ofcall.count = r->ifcall.count;
	free(cb);
	return nil;
}


static void
initfs(char *dirname)
{
	char *user;
	int i;
	char *rowname;

	user = getuser();
	s.tree = alloctree(user, user, 0555, nil);
	if(s.tree == nil)
		sysfatal("initfs: alloctree: %r");
	root = s.tree->root;
	if((devdir = createfile(root, dirname, user, DMDIR|0555, nil)) == nil)
		sysfatal("initfs: createfile: %s: %r", dirname);
	for(i = 0; i < (rows + 1); i++)
		if(createfile(devdir, files[i].name, user, files[i].mode, files + i) == nil)
			sysfatal("initfs: createfile: %s: %r", files[i].name);
}


void
threadmain(int argc, char *argv[])
{
	char *srvname, *mntpt, *i²cfile;

	srvname = "lcdfs";
	mntpt = "/mnt";
	rows = 2;
	cols = 16;
	i²cfile = "/dev/i2c1/i2c.27.data";

	ARGBEGIN {
	default:
		fprint(2, "usage: %s [-r rows] [-c columns] [-m mntpt] [-s srvname] [-d devfile]\n", argv0);
		exits("usage");
	case 's':
		srvname = ARGF();
		break;
	case 'm':
		mntpt = ARGF();
		break;
	case 'd':
		i²cfile = ARGF();
		break;
	case 'r':
		rows = strtol(ARGF(), nil, 0);
		break;
	case 'c':
		cols = strtol(ARGF(), nil, 0);
		break;
	} ARGEND

	/*
	 *	do some checks on rows and cols input,
	 *	assumeing these things tops out at
	 *	4 rows, and stopping at 40 cols because
	 *	that is where they are set to print on 
	 *	the next row
	 */

	if(rows < 1)
		rows = 1;
	if(rows > 4)
		rows = 4;
	if(cols > 40)
		cols = 40;


	if((i²cfd = open(i²cfile, ORDWR)) < 0)
		sysfatal("no i2c file");

	initfs(srvname);
	lcdinit();
	threadpostmountsrv(&s, srvname, mntpt, MBEFORE);
	threadexits(nil);
}
