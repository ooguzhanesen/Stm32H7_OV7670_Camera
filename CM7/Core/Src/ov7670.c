/*
 * ov7670.c
 *
 *  Created on: 2017/08/25
 *      Author: take-iwiw
 */
#include <stdio.h>
#include "main.h"
#include "stm32h7xx_hal.h"
#include "common.h"
#include "ov7670.h"
#include "ov7670Config.h"
#include "ov7670Reg.h"
#include "fatfs.h"

/*** Internal Const Values, Macros ***/
#define OV7670_QVGA_WIDTH  320
#define OV7670_QVGA_HEIGHT 240



uint16_t CAM_BUFFER[CAM_BUFFER_SIZE];

/*** Internal Static Variables ***/
static DCMI_HandleTypeDef *sp_hdcmi;
static DMA_HandleTypeDef  *sp_hdma_dcmi;
static I2C_HandleTypeDef  *sp_hi2c;
static uint32_t    s_destAddressForContiuousMode;
static void (* s_cbHsync)(uint32_t h);
static void (* s_cbVsync)(uint32_t v);
static uint32_t s_currentH;
static uint32_t s_currentV;

/*** Internal Function Declarations ***/
static RET ov7670_write(uint8_t regAddr, uint8_t data);
static RET ov7670_read(uint8_t regAddr, uint8_t *data);

/*** External Function Defines ***/
RET ov7670_init(DCMI_HandleTypeDef *p_hdcmi, DMA_HandleTypeDef *p_hdma_dcmi, I2C_HandleTypeDef *p_hi2c)
{
  sp_hdcmi     = p_hdcmi;
  sp_hdma_dcmi = p_hdma_dcmi;
  sp_hi2c      = p_hi2c;
  s_destAddressForContiuousMode = 0;

  HAL_GPIO_WritePin(CAMERA_RESET_GPIO_Port, CAMERA_RESET_Pin, GPIO_PIN_RESET);
  HAL_Delay(100);
  HAL_GPIO_WritePin(CAMERA_RESET_GPIO_Port, CAMERA_RESET_Pin, GPIO_PIN_SET);
  HAL_Delay(100);

  ov7670_write(0x12, 0x80);  // RESET
  HAL_Delay(30);

  uint8_t buffer[4];
  ov7670_read(0x0b, buffer);
  printf("[OV7670] dev id = %02X\n", buffer[0]);

  return RET_OK;
}

RET ov7670_config(uint32_t mode)
{
  ov7670_stopCap();
  ov7670_write(0x12, 0x80);  // RESET
  HAL_Delay(30);
  for(int i = 0; OV7670_reg[i][0] != REG_BATT; i++) {
    ov7670_write(OV7670_reg[i][0], OV7670_reg[i][1]);
    HAL_Delay(1);
  }

  return RET_OK;
}

void ov7670_config2(void)
{
	ov7670_stopCap();
	  ov7670_write(0x12, 0x80);  // RESET
	  HAL_Delay(30);
	  for(int i = 0; OV7670_reg[i][0] != REG_BATT; i++) {
	    ov7670_write(OV7670_reg[i][0], OV7670_reg[i][1]);
	    HAL_Delay(1);}

	 ov7670_write(0x12, 0x80); // COM7 - Reset
	    HAL_Delay(100);


	    // 3. RGB565 formatına ayarla
	    ov7670_write(0x12, 0x14);   // COM7 - QVGA + RGB
	    ov7670_write(0x40, 0xD0);   // COM15 - RGB565 + full range
	    ov7670_write(0x8C, 0x00);   // RGB444 - disable
	    ov7670_write(0x3A, 0x04);   // TSLB - UYVY sıralama
	    ov7670_write(0x3D, 0xC0);   // COM13 - gamma + UVSAT

	    // 4. Renk kazançlarını dengele (yeşil baskıyı azaltmak için)
	    ov7670_write(0x01, 0x50);   // BLUE gain
	    ov7670_write(0x02, 0x40);   // RED gain
	    ov7670_write(0x6A, 0x30);   // GREEN gain (daha düşük)

	    // 5. Sabit pozlama ve gain değerleri (gerekirse ayarlanabilir)
	    ov7670_write(0x00, 0x20);   // GAIN
	    ov7670_write(0x10, 0x40);   // AECH - exposure time

	    // 6. PCLK ve saat ayarları
	    ov7670_write(0x11, 0x01);   // CLKRC - PCLK divider
	    ov7670_write(0x6B, 0x2A);   // PLL ayarı

	    // 7. Görüntü boyutu (QVGA crop ayarı, isteğe bağlı)
	    ov7670_write(0x73, 0xF1);   // Scaling X
	    ov7670_write(0xA2, 0x52);   // Scaling Y
	}


RET ov7670_startCap(uint32_t capMode, uint32_t destAddress)
{

	uint8_t some_register_value = 0;
	ov7670_read(0x12, &some_register_value);

  ov7670_stopCap();
  if (capMode == OV7670_CAP_CONTINUOUS) {
    /* note: continuous mode automatically invokes DCMI, but DMA needs to be invoked manually */
    s_destAddressForContiuousMode = destAddress;
    HAL_DCMI_Start_DMA(sp_hdcmi, DCMI_MODE_CONTINUOUS, destAddress, OV7670_QVGA_WIDTH * OV7670_QVGA_HEIGHT/2);
  } else if (capMode == OV7670_CAP_SINGLE_FRAME) {
    s_destAddressForContiuousMode = 0;
    HAL_DCMI_Start_DMA(sp_hdcmi, DCMI_MODE_SNAPSHOT, destAddress, OV7670_QVGA_WIDTH * OV7670_QVGA_HEIGHT/2);
  }

  return RET_OK;
}

RET ov7670_stopCap()
{
  HAL_DCMI_Stop(sp_hdcmi);
//  HAL_Delay(30);
  return RET_OK;
}

void ov7670_registerCallback(void (*cbHsync)(uint32_t h), void (*cbVsync)(uint32_t v))
{
  s_cbHsync = cbHsync;
  s_cbVsync = cbVsync;
}

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi)
{
//  printf("FRAME %d\n", HAL_GetTick());
  if(s_cbVsync)s_cbVsync(s_currentV);
  if(s_destAddressForContiuousMode != 0) {
    HAL_DMA_Start_IT(hdcmi->DMA_Handle, (uint32_t)&hdcmi->Instance->DR, s_destAddressForContiuousMode, OV7670_QVGA_WIDTH * OV7670_QVGA_HEIGHT/2);
  }
  s_currentV++;
  s_currentH = 0;
}

void HAL_DCMI_VsyncEventCallback(DCMI_HandleTypeDef *hdcmi)
{
//  printf("VSYNC %d\n", HAL_GetTick());
//  HAL_DMA_Start_IT(hdcmi->DMA_Handle, (uint32_t)&hdcmi->Instance->DR, s_destAddressForContiuousMode, OV7670_QVGA_WIDTH * OV7670_QVGA_HEIGHT/2);
}

//void HAL_DCMI_LineEventCallback(DCMI_HandleTypeDef *hdcmi)
//{
////  printf("HSYNC %d\n", HAL_GetTick());
//  if(s_cbHsync)s_cbHsync(s_currentH);
//  s_currentH++;
//}

/*** Internal Function Defines ***/
static RET ov7670_write(uint8_t regAddr, uint8_t data)
{
  HAL_StatusTypeDef ret;
  do {
    ret = HAL_I2C_Mem_Write(sp_hi2c, SLAVE_ADDR, regAddr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);
    ret = HAL_I2C_Master_Transmit(sp_hi2c, SLAVE_ADDR, &regAddr, 1, 100);
  } while (ret != HAL_OK && 0);
  return ret;
}

 RET ov7670_read(uint8_t regAddr, uint8_t *data)
{
  HAL_StatusTypeDef ret;
  do {
    // HAL_I2C_Mem_Read doesn't work (because of SCCB protocol(doesn't have ack))? */
//    ret = HAL_I2C_Mem_Read(sp_hi2c, SLAVE_ADDR, regAddr, I2C_MEMADD_SIZE_8BIT, data, 1, 1000);
    ret = HAL_I2C_Master_Transmit(sp_hi2c, SLAVE_ADDR, &regAddr, 1, 100);
    ret |= HAL_I2C_Master_Receive(sp_hi2c, SLAVE_ADDR, data, 1, 100);
  } while (ret != HAL_OK && 0);
  return ret;
}

 void writeBMPfile(void)
 {
     FATFS fs;
     FIL file;
     FRESULT res;
     UINT bw;
     char filename[20];

     // 320x240 RGB565 BMP header + Color Masks
     static const uint8_t bmp_header[66] = {
         // BMP Header (14 byte)
         0x42, 0x4D,             // Signature 'BM'
         0x36, 0x6C, 0x02, 0x00, // File size = 0x00026C36 = 155,830 bytes
         0x00, 0x00,             // Reserved
         0x00, 0x00,             // Reserved
         0x46, 0x00, 0x00, 0x00, // Offset to pixel data = 70

         // DIB Header (40 byte)
         0x28, 0x00, 0x00, 0x00, // DIB header size
         0x40, 0x01, 0x00, 0x00, // Width: 320
         0xF0, 0x00, 0x00, 0x00, // Height: 240
         0x01, 0x00,             // Color planes
         0x10, 0x00,             // Bits per pixel: 16
         0x03, 0x00, 0x00, 0x00, // Compression: BI_BITFIELDS
         0xF0, 0x6B, 0x02, 0x00, // Image size = 0x00026BF0 = 155,760
         0x13, 0x0B, 0x00, 0x00, // X pixels per meter
         0x13, 0x0B, 0x00, 0x00, // Y pixels per meter
         0x00, 0x00, 0x00, 0x00, // Colors in color table
         0x00, 0x00, 0x00, 0x00, // Important color count

         // Color masks (12 byte)
         0x00, 0xF8, 0x00, 0x00, // Red mask
         0xE0, 0x07, 0x00, 0x00, // Green mask
         0x1F, 0x00, 0x00, 0x00  // Blue mask
     };

     // Frame buffer kaynağın (örnek: SDRAM)

     res = f_mount(&fs, "", 0);
     if (res == FR_OK)
     {
         sprintf(filename, "%lu.bmp", HAL_GetTick()); // Dosya ismi
         res = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
         if (res == FR_OK)
         {
             res = f_write(&file, bmp_header, sizeof(bmp_header), &bw);
             if (res == FR_OK)
             {

                 // BMP formatı ters satır sırası ister (bottom-up)
                 for (int y = 239; y >= 0; y--) {
                     for (int x = 0; x < 320; x++) {
                         uint16_t pixel = CAM_BUFFER[y * 320 + x];
                         f_write(&file, &pixel, 2, &bw);
                     }
                 }
             }
             f_close(&file);
         }
     }
 }
