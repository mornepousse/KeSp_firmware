#pragma once
void GC9A01_Initial(void);
void LCD_SetPos(unsigned int Xstart,unsigned int Ystart,unsigned int Xend,unsigned int Yend);
void ClearScreen(unsigned int bColor);
void Draw_Circle(unsigned int x0,unsigned int y0,unsigned char r,unsigned int color);
void LCD_DrawRectangle(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2,unsigned int color);
void LCD_DrawLine(unsigned int x1,unsigned int y1,unsigned int x2,unsigned int y2,unsigned int color);
void LCD_DrawPoint(unsigned int x,unsigned int y,unsigned int color);
void showzifustr(unsigned int x,unsigned int y,unsigned char *str,unsigned int dcolor,unsigned int bgcolor);
void showzifu(unsigned int x,unsigned int y,unsigned char value,unsigned int dcolor,unsigned int bgcolor);
void show_picture(void);
void DispRGBGray(void);