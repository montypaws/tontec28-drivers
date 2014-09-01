#include "bcm2835.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/timeb.h>
#include <time.h>

/* Global State for Graceful Exits */
char state = 0;

#include "lcd.h"
#include "framebuffer.h"

void setsample(int *r, int *g, int *b, framebuffer *fb, unsigned long offset){
	unsigned char *buf = &fb->buffer[offset];
	int p = (buf[1] << 8) | buf[0];

	*r += (p & RGB565_MASK_RED) >> 10;
	*g += (p & RGB565_MASK_GREEN) >> 5;
	*b += (p & RGB565_MASK_BLUE) << 1;
}

int downscale(framebuffer *fb, int x, int y){
	unsigned long offset = 0;
	int r = 0, g = 0, b = 0;

	offset = (y * 2 * DISP_W + x) * 2;
	setsample(&r, &g, &b, fb, offset);

	switch(fb->mode){
	case 3:
	case 5:
	case 7:
		// Ugly single-sample mode
		r <<= 2;
		g <<= 2;
		b <<= 2;
		break;

	default:
		// Blended sample mode.
		offset += 2;
		setsample(&r, &g, &b, fb, offset);

		offset += 4 * DISP_W;
		setsample(&r, &g, &b, fb, offset);

		offset -= 2;
		setsample(&r, &g, &b, fb, offset);
	}

	return RGB565(r, g, b);
}

void loadFrameBuffer_diff(framebuffer *fb){
	int diffsx, diffsy, diffex, diffey;
	unsigned long offset = 0;
	int interlace_set = 0;
	long diff_count;
	long diff_pos;
	int i, j, p;

	long dtime;
	struct timespec time_last, time_cur;
	time_last = (struct timespec){ 0, 0 };

	framebuffer_read(fb);

	while(!state){
		clock_gettime(CLOCK_REALTIME, &time_cur);
		if(time_cur.tv_sec - time_last.tv_sec <= 1){
			dtime = 100000L - ((time_cur.tv_sec - time_last.tv_sec) * 1e6L + (time_cur.tv_nsec - time_last.tv_nsec) / 1e3L);

			// Delay to save CPU
			if(dtime > 0)
				usleep(dtime);
		}
		time_last = time_cur;

		// Switch buffer planes
		fb->flag = 1 - fb->flag;

		// Swap interlace scanlines
		interlace_set = 1 - interlace_set;

		diff_pos = 0L;
		diff_count = 0L;
		diffex = diffey = 0;
		diffsx = diffsy = 65535;

		for(i = 0; i <= MAX_Y; i++){
			if((fb->mode == 4) && (i % 2 == interlace_set))
				continue;
			else if((fb->mode == 6) || (fb->mode == 7))
				interlace_set = 1 - interlace_set;

			for(j = 0; j <= MAX_X; j++){
				switch(fb->mode){
					case 2:
						offset = (i * DISP_W + j) * 2;
						p = (fb->buffer[offset + 1] << 8) | fb->buffer[offset];
						break;

					case 6:
					case 7:
						// Checkerboard mode
						interlace_set = 1 - interlace_set;
						if(interlace_set)
							continue;
					default:
						p = downscale(fb, j << 1, i << 1);
						break;
				}

				if(fb->drawmap[1 - fb->flag][i][j] != p) {
					fb->drawmap[fb->flag][i][j] = p;
					fb->drawmap[1 - fb->flag][i][j] = p;

					// Maintain a small list of diff coords.
					if(diff_count++ < DOTTED_DRAW_SIZE){
						fb->diffmap[0][diff_pos] = i;
						fb->diffmap[1][diff_pos] = j;
						diff_pos++;
					}

					if(i < diffsx)
						diffsx = i;
					if(i > diffex)
						diffex = i; 
					if(j < diffsy)
						diffsy = j;
					if(j > diffey)
						diffey = j;
				}
			}
		}

		// Signal the end of the diff block.
		if(diff_pos < DOTTED_DRAW_SIZE)
			fb->diffmap[0][diff_pos] = -1;

		// Diff block magic. Changed coordinates are saved in memory.
		if(diff_count && (diff_count <= DOTTED_DRAW_SIZE))
			for(diff_pos = 0; ((i = fb->diffmap[0][diff_pos]) != -1) && (diff_pos < DOTTED_DRAW_SIZE); diff_pos++)
				write_dot(fb->invert ? (MAX_Y - i) : i, ((j = fb->diffmap[1][diff_pos]), fb->invert ? (MAX_X - j) : j), fb->drawmap[fb->flag][i][j]);
		else {
			// Large area writes are faster than single dots. Needed for big delta.
			LCD_WR_CMD(YS, diffsx); // Column address start
			LCD_WR_CMD(YE, diffex); // Column address end
			LCD_WR_CMD(XS, diffsy); // Row address start
			LCD_WR_CMD(XE, diffey); // Row address end

			LCD_WR_CMD(XP, diffsy); // Column address start
			LCD_WR_CMD(YP, diffsx); // Row address start

			LCD_WR_REG(0x22);
			LCD_CS_CLR;
			LCD_RS_SET;

			for(i = diffsx; i <= diffex; i++)
				for(j = diffsy; j <= diffey; j++)
					LCD_WR_Data(fb->drawmap[fb->flag][fb->invert ? (MAX_Y - i) : i][fb->invert ? (MAX_X - j) : j]);
		}
		framebuffer_read(fb);
	}
}

/* Sets the state variable true, which should signal framebuffer loops to exit
 * when convenient. */
void gracefulexit(int na){
	state |= 0x1;
}

int main(int argc, char **argv){
	int displayMode = 0;
	int c, state = 0;
	int runTest = 0;
	int invert = 0;

	while(!state && (c = getopt(argc, argv, "1234567tv"))) switch(c){
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
			displayMode = c - '0';
			break;
		case 't':
			runTest = 1;
			break;
		case 'v':
			invert = 1;
			break;

		case -1:
			state = 1;
			break;
		default:
			state = 2;
	}
	if(state == 2){
		fprintf(stderr,
			"Usage:\t%s [flags]\n"
			"\t1 - downsample 640x480 mode (default)\n"
			"\t2 - native 320x240 mode\n"
			"\t3 - ugly/fast downsample 640x480 mode\n"
			"\t4 - downsample interlaced 640x480 mode\n"
			"\t5 - uglier/faster downsample interlaced 640x480 mode\n"
			"\t6 - checkerboard downsample interlaced 640x480 mode\n"
			"\t7 - ugliest/fastest checkerboard downsample interlaced 640x480 mode\n"
			"\tv - \"Invert Mode,\" rotate screen 180 degrees\n"
			"\tt - Run display test at startup\n"
			"\n",
			*argv
		);

		return 0;
	}

	if(!bcm2835_init()){
		printf("bcm2835 init error\n");
		return 1;
	}

	bcm2835_gpio_fsel(SPICS, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_fsel(SPIRS, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_fsel(SPIRST, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_fsel(LCDPWM, BCM2835_GPIO_FSEL_OUTP);

	bcm2835_spi_begin();
	bcm2835_spi_setBitOrder(BCM2835_SPI_BIT_ORDER_MSBFIRST);
	bcm2835_spi_setDataMode(BCM2835_SPI_MODE3);
	bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_2);

	LCD_PWM_CLR;
	LCD_Init();

	if(runTest)
		LCD_test();

	signal(SIGUSR1, gracefulexit);

	// OR in options (i.e., FRAMEBUFFER_INVERT_VIDEO)
	loadFrameBuffer_diff(framebuffer_create(displayMode | (invert  ? FRAMEBUFFER_INVERT_VIDEO : 0), "/dev/fb0"));

	return 0;
}
