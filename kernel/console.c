// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void consputc(int);

static int panicked = 0;


int selectedRow = 2;
// Left => 0 ; Right => 1
int selectedCol = 0;
int tableActive = 0;

int fgColorConst = 4096;
int bgColorConst = 256;
int isLightConstFg = 2048;
int isLightConstBg = 32768;

int isLightFg = 0;
int isLightBg = 0;


int selectedBgClr = 7;
int selectedFgClr = 0;


static struct {
	struct spinlock lock;
	int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
	static char digits[] = "0123456789abcdef";
	char buf[16];
	int i;
	uint x;

	if(sign && (sign = xx < 0))
		x = -xx;
	else
		x = xx;

	i = 0;
	do{
		buf[i++] = digits[x % base];
	}while((x /= base) != 0);

	if(sign)
		buf[i++] = '-';

	while(--i >= 0)
		consputc(buf[i]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
	int i, c, locking;
	uint *argp;
	char *s;

	locking = cons.locking;
	if(locking)
		acquire(&cons.lock);

	if (fmt == 0)
		panic("null fmt");

	argp = (uint*)(void*)(&fmt + 1);
	for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
		if(c != '%'){
			consputc(c);
			continue;
		}
		c = fmt[++i] & 0xff;
		if(c == 0)
			break;
		switch(c){
		case 'd':
			printint(*argp++, 10, 1);
			break;
		case 'x':
		case 'p':
			printint(*argp++, 16, 0);
			break;
		case 's':
			if((s = (char*)*argp++) == 0)
				s = "(null)";
			for(; *s; s++)
				consputc(*s);
			break;
		case '%':
			consputc('%');
			break;
		default:
			// Print unknown % sequence to draw attention.
			consputc('%');
			consputc(c);
			break;
		}
	}

	if(locking)
		release(&cons.lock);
}

void
panic(char *s)
{
	int i;
	uint pcs[10];

	cli();
	cons.locking = 0;
	// use lapiccpunum so that we can call panic from mycpu()
	cprintf("lapicid %d: panic: ", lapicid());
	cprintf(s);
	cprintf("\n");
	getcallerpcs(&s, pcs);
	for(i=0; i<10; i++)
		cprintf(" %p", pcs[i]);
	panicked = 1; // freeze other CPU
	for(;;)
		;
}

#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
	int pos;

	// Cursor position: col + 80*row.
	outb(CRTPORT, 14);
	pos = inb(CRTPORT+1) << 8;
	outb(CRTPORT, 15);
	pos |= inb(CRTPORT+1);

	if(c == '\n')
		pos += 80 - pos%80;
	else if(c == BACKSPACE){
		if(pos > 0) --pos;
	} else
		crt[pos++] = (c&0xff) | (bgColorConst * selectedBgClr + fgColorConst * selectedFgClr + isLightFg * isLightConstFg + isLightBg * isLightConstBg); ;  // black on white

	if(pos < 0 || pos > 25*80)
		panic("pos under/overflow");

	if((pos/80) >= 24){  // Scroll up.
		memmove(crt, crt+80, sizeof(crt[0])*23*80);
		pos -= 80;
		memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
	}

	outb(CRTPORT, 14);
	outb(CRTPORT+1, pos>>8);
	outb(CRTPORT, 15);
	outb(CRTPORT+1, pos);
	crt[pos] = ' ' | (bgColorConst * selectedBgClr + fgColorConst * selectedFgClr + isLightFg * isLightConstFg + isLightBg * isLightConstBg);
}

void
consputc(int c)
{
	if(panicked){
		cli();
		for(;;)
			;
	}

	if(c == BACKSPACE){
		uartputc('\b'); uartputc(' '); uartputc('\b');
	} else
		uartputc(c);
	cgaputc(c);
}

#define INPUT_BUF 128
struct {
	char buf[INPUT_BUF];
	uint r;  // Read index
	uint w;  // Write index
	uint e;  // Edit index
} input;

#define C(x)  ((x)-'@')  // Control-x



// 24x10
// /---<FG>--- ---<BG>---\
// |Black     |Black     |
// |Blue      |Blue      |
// |Green     |Green     |
// |Aqua      |Aqua      |
// |Red       |Red       |
// |Purple    |Purple    |
// |Yellow    |Yellow    |
// |White     |White     |
// \---------------------/




int drawTable() {
	drawTableRow("/---<BG>--- ---<FG>---\\", 80);
	drawTableRow("|Black     |Black     |", 80*2);
	drawTableRow("|Blue      |Blue      |", 80*3);
	drawTableRow("|Green     |Green     |", 80*4);
	drawTableRow("|Aqua      |Aqua      |", 80*5);
	drawTableRow("|Red       |Red       |", 80*6);
	drawTableRow("|Purple    |Purple    |", 80*7);
	drawTableRow("|Yellow    |Yellow    |", 80*8);
	drawTableRow("|White     |White     |", 80*9);
	drawTableRow("\\---------------------/", 80*10);	
  
}

void drawTableRow(char* str, int rowEnd) {

	if(rowEnd == selectedRow * 80) {

		if(selectedCol == 0) makeLeftClmnActive(str,rowEnd);

		else makeRightClmnActive(str,rowEnd);
		
	} else {
		for(int i = rowEnd - strlen(str); i < rowEnd; i++) crt[i] = str[i - rowEnd + strlen(str)] | 0x0700;
	}

} 

void makeLeftClmnActive(char* str, int start){
	for(int i = start - strlen(str); i < start - strlen(str) / 2 - 1; i++) crt[i] = str[i - start + strlen(str)] | 0x7000;
	for(int i = start - strlen(str) / 2 - 1; i < start; i++) crt[i] = str[i - start + strlen(str)] | 0x0700;
}

void makeRightClmnActive(char* str, int start) {
	for(int i = start - strlen(str); i < start - strlen(str) / 2 - 1; i++) crt[i] = str[i - start + strlen(str)] | 0x0700;
	for(int i = start - strlen(str) / 2 - 1; i < start; i++) crt[i] = str[i - start + strlen(str)] | 0x7000;
}


// Niz za privremeno cuvanje video memorije
int tempDeletedTable[10*24];

// Kopira niz karaktera u zavisnosti od aktivnosti tabele
void copyShowingArray() {
	int end = 80;
	int newRow = 0;
	int j = end - 23;
	
	for(int i = 0; i < 240; i++){

		if(tableActive) {

			crt[j] = tempDeletedTable[i] | (bgColorConst * selectedBgClr + fgColorConst * selectedFgClr + isLightFg * isLightConstFg + isLightBg * isLightConstBg);
		} 
		else tempDeletedTable[i] = crt[j] & 0x00FF;
	
		if(newRow == 23) {
			j = j + 80 - 23;
			newRow = 0;
		} else {
			j++;
			newRow++;
		}

	}
}

// color	bg	fg
// black	4096*0	256*0
// blue		4096*1	256*1
// ...



void changeColor() {

	int colorIndex = selectedRow - 2;

	if(selectedCol == 0) {
		selectedFgClr = colorIndex;
		isLightBg = 0;
	} else {
		selectedBgClr = colorIndex;
		isLightFg = 0;
	}

	Colorize();
	drawTable();
	
}


void changeLight() {
	if(selectedCol == 0) {
		isLightBg = 1;

		 
	} else {
		isLightFg = 1;

	}
	Colorize();
	drawTable();
}


void Colorize() {
	for(int i = 0; i < 80*25; i++){
		crt[i] = crt[i] & 0x00FF;
		crt[i] = crt[i] | (bgColorConst * selectedBgClr + fgColorConst * selectedFgClr + isLightFg * isLightConstFg + isLightBg * isLightConstBg); 
	}
}


void
consoleintr(int (*getc)(void))
{
	int c, doprocdump = 0;

	acquire(&cons.lock);
	while((c = getc()) >= 0){

		if(tableActive) {

			switch(c) {

				case 'w':

					if(selectedRow == 2) selectedRow = 9;
					else selectedRow--;
					drawTable();
					break;
				case 's':

					if(selectedRow == 9) selectedRow = 2;
					else selectedRow++;	
					drawTable();		

					break;
				case 'a':
					selectedCol = !selectedCol;	
					drawTable();
					break;
				case 'd':
					selectedCol = !selectedCol;
					drawTable();
					break;

				case 'e':
					changeColor();		
					break;
				case 'r':

					changeLight();		
					break;


						

			}

		
		}

		switch(c){
		case C('P'):  // Process listing.
			// procdump() locks cons.lock indirectly; invoke later
			doprocdump = 1;
			break;
		case C('U'):  // Kill line.
			while(input.e != input.w &&
			      input.buf[(input.e-1) % INPUT_BUF] != '\n'){
				input.e--;
				consputc(BACKSPACE);
			}
			break;
		case C('H'): case '\x7f':  // Backspace
			if(input.e != input.w){
				input.e--;
				consputc(BACKSPACE);
			}
		break;

		
		// Toggle table
		case C('B'):
			copyShowingArray();
			if(tableActive) copyShowingArray();
			else drawTable();
			tableActive = !tableActive;
			break;


		
		default:
			if(c != 0 && input.e-input.r < INPUT_BUF && !tableActive){
				c = (c == '\r') ? '\n' : c;
				input.buf[input.e++ % INPUT_BUF] = c;
				consputc(c);
				if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
					input.w = input.e;
					wakeup(&input.r);
				}
			}
			break;
		}
	}
	release(&cons.lock);
	if(doprocdump) {
		procdump();  // now call procdump() wo. cons.lock held
	}
}

int
consoleread(struct inode *ip, char *dst, int n)
{
	uint target;
	int c;

	iunlock(ip);
	target = n;
	acquire(&cons.lock);
	while(n > 0){
		while(input.r == input.w){
			if(myproc()->killed){
				release(&cons.lock);
				ilock(ip);
				return -1;
			}
			sleep(&input.r, &cons.lock);
		}
		c = input.buf[input.r++ % INPUT_BUF];
		if(c == C('D')){  // EOF
			if(n < target){
				// Save ^D for next time, to make sure
				// caller gets a 0-byte result.
				input.r--;
			}
			break;
		}
		*dst++ = c;
		--n;
		if(c == '\n')
			break;
	}
	release(&cons.lock);
	ilock(ip);

	return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
	int i;

	iunlock(ip);
	acquire(&cons.lock);
	for(i = 0; i < n; i++)
		consputc(buf[i] & 0xff);
	release(&cons.lock);
	ilock(ip);

	return n;
}

void
consoleinit(void)
{
	initlock(&cons.lock, "console");

	devsw[CONSOLE].write = consolewrite;
	devsw[CONSOLE].read = consoleread;
	cons.locking = 1;

	ioapicenable(IRQ_KBD, 0);
}

