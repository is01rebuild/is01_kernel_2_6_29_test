/*
 * Copyright (C) 2009 SHARP CORPORATION
 * Copyright (C) 2009 Yamaha CORPORATION
 * Copyright (C) 2011 RO178 is01rebuild
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#if 0
#include <asm/arch/mx31_pins.h>
#include <asm/arch/gpio.h>
#else
#include <mach/gpio.h>
#endif
#include <linux/vmalloc.h>
#include <linux/string.h>
#include "madrv.h"

#define PERIOD 100 // ioctl cmd のprintk実行周期 ( 総実行回数 % PERIOD ) ==0 で出力
#define DEBUG_DUMP_START                 0
#define DEBUG_DUMP_END               20000 // sizeof(struct cmd_dump)
#define DEBUG_DUMP_READ_MAX          20000 // sizeof(struct ma_IoCtlReadRegWait)
#define DEBUG_DUMP_WRITE_MAX         20000 // sizeof(struct ma_IoCtlWriteRegWait)

#define DEBUG_DATA_DUMP_WRITE_MAX   100000 // unsigned char u_int8 and unsigned short u_int16
#define DEBUG_DATA_DUMP_READ_MAX    100000  // unsigned char u_int8 and unsigned short u_int16

//最小出力msg
#define DEBUG(fmt, args...) printk(KERN_INFO "yamaha:%s(): " fmt "\n", __FUNCTION__  ,##args)

// on/off可能msg
#if 0
#define DK(fmt, args...) printk( fmt, ##args)
#else
#define DK(fmt, args...) do {} while (0)
#endif

#if 0
#define D(fmt, args...) printk(KERN_INFO "YMU:%s(): " fmt, __FUNCTION__  ,##args)
#define DI(fmt, args...) printk(KERN_INFO "//YMU:IO" fmt, ##args)
#define KDEBUG_FUNC() printk("yamaha: ae2drv: %s()\n", __FUNCTION__)
#else
#define D(fmt, args...) do {} while (0)
#define DI(fmt, args...) do {} while (0)
#define KDEBUG_FUNC() do {} while (0)
#endif

#define YAMAHA_DEBUG_LOG 1

#define MA_DEVICE_NODE_NAME	"ae2"
#define MA_DEVICE_NAME 	"ae2"
#define MA_DEVICE_IRQ_NAME	"irq_ae2"
#define MA_DEVICE_COUNT	(1)
#define MA_CLASS_NAME  	"cls_ae2"

extern void gpio_ext_bus_active(void);

struct MaDriverInfo
{
                void         	*pMemory;
	unsigned int		dIrq;
	struct cdev		sCdev;
	wait_queue_head_t	sQueue;
	spinlock_t		sLock;
	unsigned int		dIrqCount;
	unsigned int		dMaskIrq;
	unsigned int		dCanceled;
};

static char* cmdToText[10] ={ "MA_IOCTL_WAIT",
	       "MA_IOCTL_SLEEP",
	       "MA_IOCTL_WRITE_REG_WAIT",
	       "MA_IOCTL_READ_REG_WAIT",
	       "MA_IOCTL_DISABLE_IRQ",
	       "MA_IOCTL_ENABLE_IRQ",
	       "MA_IOCTL_RESET_IRQ_MASK_COUNT",
	       "MA_IOCTL_WAIT_IRQ",
	       "MA_IOCTL_CANCEL_WAIT_IRQ",
	       "MA_IOCTL_SET_GPIO" };

static int ma_IoCtl( struct inode *psInode, struct file *psFile, unsigned int dCmd, unsigned long dArg );
static int ma_Open( struct inode *psInode, struct file *psFile );
static int ma_Close( struct inode *psInode, struct file *psFile );

static int ma_OpenDebug( struct inode *psInode, struct file *psFile );
static int ma_CloseDebug( struct inode *psInode, struct file *psFile );
static ssize_t ma_ReadDebug( struct file* psFile, char* buf, size_t count, loff_t* pos );

static struct file_operations ma_FileOps =
{
  .owner = THIS_MODULE,
  .ioctl = ma_IoCtl,
  .open = ma_Open,
  .release = ma_Close,
};

static struct file_operations ma_FileOpsDebug =
{
  .owner = THIS_MODULE,
  .read    = ma_ReadDebug,
  //.write   = NULL,
  .open = ma_OpenDebug,
  .release = ma_CloseDebug,
};

static struct MaDriverInfo *gpsDriver = NULL;
static int gsMajor = -1;
static dev_t gsDev;

static struct class *gpsClass = NULL;

//----
static struct cdev sCdevDebug;
static struct cmd_dump {
    unsigned int cmd;
    unsigned int arg;
} *dump=NULL;
static struct ma_IoCtlReadRegWait *dumpRead=NULL;
static struct ma_IoCtlWriteRegWait *dumpWrite=NULL;
static unsigned char  *dumpWriteByte = NULL;
static unsigned short *dumpWriteWord = NULL;
static unsigned char  *dumpReadByte = NULL;
static unsigned short *dumpReadWord = NULL;

static int readCount=0;
static int readDataMode=0;
static unsigned int readDataCount=0;
static int irqFlag=0;

static struct ioctl_count {
    unsigned int allCount;

    unsigned int waitCount;//pos uint
    unsigned int sleepCount;//pos uint
    unsigned int writeCount,writeByteCount,writeWordCount;//pos struct ma_IoCtlWriteRegWait
    unsigned int readCount,readByteCount,readWordCount;//pos struct ma_IoCtlReadRegWait
    unsigned int disableCount;//-
    unsigned int enableCount;//-
    unsigned int resetCount;//-
    unsigned int waitIRQCount;//pos int
    unsigned int cancelCount;//-
    unsigned int setCount;//pos unsigned int

} ReadWriteCount ={ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

/****************************************************************************
 *	NanoWait
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
NanoWait( unsigned int dPeriod )
{
	unsigned int ms = 0, us = 0, ns = 0;
	//KDEBUG_FUNC();
	ms = dPeriod / 1000000;
	us = ( dPeriod - ( ms * 1000000 ) ) / 1000;
	ns = dPeriod - ( ms * 1000000 ) - ( us * 1000 );
	if ( ms > 0 )
	{
		mdelay( ms );
	}
	if ( us > 0 )
	{
		udelay( us );
	}
	if ( ns > 0 )
	{
		ndelay( ns );
	}
	return 0;
}

/****************************************************************************
 *	IoCtl_Wait
 *
 *	Description:
 *			Processing that does weight every Nano second is done. 
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	wait value(ns)
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_Wait( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		Processing that does weight every Nano second is done.
	*/

	(void)psInode;
	(void)psFile;
	//KDEBUG_FUNC();
	return NanoWait( dArg );
}

/****************************************************************************
 *	MilliSleep
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
#if (0)
static inline int
MilliSleep( unsigned int dPeriod )
{
  //KDEBUG_FUNC();
	msleep( dPeriod );
	return 0;
}

#else
static inline int
MilliSleep( unsigned int dPeriod )
{
	struct timeval sStart, sCurrent;
	unsigned long dDiff = 0;
	//KDEBUG_FUNC();
	if ( dPeriod == 0 )
	{
		schedule();
		return 0;
	}
	if ( dPeriod > 4000000 )
	{
		return -EINVAL; /* Too big */
	}
	/* do_gettimeofday might provide more accuracy than jiffies */
	do_gettimeofday( &sStart );
	for ( ;; )
	{
		do_gettimeofday( &sCurrent );
		dDiff = ( sCurrent.tv_sec - sStart.tv_sec ) * 1000000 + ( sCurrent.tv_usec - sStart.tv_usec );
		if ( dDiff > dPeriod * 1000 )
		{
			break;
		}
		schedule();
	}
	return 0;
}
#endif

/****************************************************************************
 *	IoCtl_Sleep
 *
 *	Description:
 *			Processing that does the sleep in each millisecond is done. 
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	sleep value(ms)
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_Sleep( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		Processing that does the sleep in each millisecond is done.
	*/

	(void)psInode;
	(void)psFile;
	(void)dArg;
	//KDEBUG_FUNC();
	return MilliSleep( dArg );
}

/****************************************************************************
 *	WriteRegWait
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
WriteRegWait( unsigned long dAddress, void *pData, unsigned int dSize, unsigned int dWait )
{
	(void)dWait;
	//KDEBUG_FUNC();
	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
#if 0
	dAddress <<= 1;
#endif
	dAddress += (unsigned long) gpsDriver->pMemory;
#if 1
	NanoWait( dWait );
#endif
	switch ( dSize )
	{
	case sizeof( unsigned char ):
		iowrite8( *(unsigned char *)pData, (void *)dAddress );
		break;
	case sizeof( unsigned short ):
		iowrite16( *(unsigned short *)pData, (void *)dAddress );
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/****************************************************************************
 *	IoCtl_WriteRegWait
 *
 *	Description:
 *			The processing written in the interface register is done. 
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	pointer to ma_IoCtlWriteRegWait structure
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_WriteRegWait( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		Data is copied from the user space, and it writes it in the interface register.
	*/

	struct ma_IoCtlWriteRegWait sParam;
	unsigned char bData = 0;
	unsigned short wData = 0;
#if 1
	unsigned long dFlags = 0;
#else
	unsigned int dFlags = 0;
#endif
	unsigned int dCnt;

	//KDEBUG_FUNC();

	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	// ユーザランドより ma_IoCtlWriteRegWait 構造体データを カーネル領域にコピー
	if ( copy_from_user( &sParam, (void*)dArg, sizeof( sParam ) ) )
	{
		return -EFAULT;
	}
	switch ( sParam.dSize ) {
	case sizeof( unsigned char ):
		for(dCnt=0; dCnt<sParam.dDataLen; ++dCnt) {
			get_user( bData, (((unsigned char*)sParam.pData) + dCnt) );
			spin_lock_irqsave( &gpsDriver->sLock, dFlags);
			WriteRegWait( sParam.dAddress, &bData, sizeof( bData ), sParam.dWait );
			spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
		}
		break;
	case sizeof( unsigned short ):
		for(dCnt=0; dCnt<sParam.dDataLen; ++dCnt) {
			get_user( wData, (((unsigned short*)sParam.pData) + dCnt) );
			spin_lock_irqsave( &gpsDriver->sLock, dFlags);
			WriteRegWait( sParam.dAddress, &wData, sizeof( wData ), sParam.dWait );
			spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static inline int
IoCtl_WriteRegWait_DEBUG( int allCount , struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		Data is copied from the user space, and it writes it in the interface register.
	*/

	struct ma_IoCtlWriteRegWait sParam;
	unsigned char bData = 0;
	unsigned short wData = 0;
	unsigned int dCnt;

	// ユーザランドより ma_IoCtlWriteRegWait 構造体データを カーネル領域にコピー
	if ( copy_from_user( &sParam, (void*)dArg, sizeof( sParam ) ) )
	{
		return -EFAULT;
	}

    if( ReadWriteCount.writeCount < DEBUG_DUMP_WRITE_MAX ) {
        dumpWrite[ReadWriteCount.writeCount].dAddress =        sParam.dAddress;
        dumpWrite[ReadWriteCount.writeCount].dSize =           sParam.dSize;
        dumpWrite[ReadWriteCount.writeCount].dDataLen =        sParam.dDataLen;
        dumpWrite[ReadWriteCount.writeCount].dWait =           sParam.dWait;

        dump[allCount].arg = ReadWriteCount.writeCount;
    }

    DK(KERN_INFO "dumpWrite[%d].dAddress=%lu;\n",ReadWriteCount.writeCount,sParam.dAddress); //  I/F Address
    DK(KERN_INFO "dumpWrite[%d].pData=0x%p;\n",ReadWriteCount.writeCount,sParam.pData); // Write Pointer 
    DK(KERN_INFO "dumpWrite[%d].dSize=%u;\n",ReadWriteCount.writeCount,sParam.dSize); //  Write Size(data type size)
    DK(KERN_INFO "dumpWrite[%d].dDataLen=%u;\n",ReadWriteCount.writeCount,sParam.dDataLen); //  Data Length
    DK(KERN_INFO "dumpWrite[%d].dWait=%u;\n",ReadWriteCount.writeCount,sParam.dWait); //  Wait ns

	switch ( sParam.dSize ) {
	case sizeof( unsigned char ):
          dumpWrite[ReadWriteCount.writeCount].pData = (void *)ReadWriteCount.writeByteCount;
	  for(dCnt=0;dCnt<sParam.dDataLen; ++dCnt) {
	    if(  ReadWriteCount.writeByteCount  < DEBUG_DATA_DUMP_WRITE_MAX ) {
	      get_user( bData, (((unsigned char*)sParam.pData) + dCnt) );
	      DK(KERN_INFO "dumpWrite[%d] dumpWriteByte[%d]=0x%02x;\n" ,ReadWriteCount.writeCount ,ReadWriteCount.writeByteCount ,bData );
              dumpWriteByte[ReadWriteCount.writeByteCount]=bData;
              ReadWriteCount.writeByteCount++;
	    }
	  }
	  break;
	case sizeof( unsigned short ):
          dumpWrite[ReadWriteCount.writeCount].pData = (void *)ReadWriteCount.writeWordCount;
	  for(dCnt=0; dCnt<sParam.dDataLen; ++dCnt) {
	    if(  ReadWriteCount.writeWordCount  < DEBUG_DATA_DUMP_WRITE_MAX ) {
	      get_user( wData, (((unsigned short*)sParam.pData) + dCnt) );
	      DK(KERN_INFO "dumpWrite[%d] dumpWriteWord[%d]=0x%04x;\n" ,ReadWriteCount.writeCount ,ReadWriteCount.writeWordCount ,wData );
              dumpWriteWord[ReadWriteCount.writeWordCount]=wData;
              ReadWriteCount.writeWordCount++;
	    }
	  }
	  break;
	default:
	  return -EINVAL;
	}
	
	return 0;
}


/****************************************************************************
 *	ReadRegWait
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
ReadRegWait( unsigned long dAddress, void *pData, unsigned int dSize, unsigned int dWait )
{
  //KDEBUG_FUNC();
	/*
		It reads it from the interface register.
		The read value is copied onto the user space.
	*/

	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
#if 0
	dAddress <<= 1;
#endif
	dAddress += (unsigned long) gpsDriver->pMemory;
	NanoWait( dWait );
	switch ( dSize ) {
	case sizeof( unsigned char ):
		*(unsigned char *)pData = ioread8( (void *)dAddress );
		//DK("pData=%02x\n",*(unsigned char *)pData);
		break;
	case sizeof( unsigned short ):
		*(unsigned short *)pData = ioread16( (void *)dAddress );
		//DK("pData=%04x\n",*(unsigned short *)pData);
                break;
	default:
		return -EINVAL;
	}
	return 0;
}

/****************************************************************************
 *	IoCtl_ReadRegWait
 *
 *	Description:
 *			The processing read from the interface register is done. 
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	pointer to ma_IoCtlReadRegWait structure
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_ReadRegWait( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		It reads it from the interface register.
		The read value is copied onto the user space.
	*/

	struct ma_IoCtlReadRegWait sParam;
	unsigned char bData = 0;
	unsigned short wData = 0;
#if 1
	unsigned long dFlags;
#else
	unsigned int dFlags;
#endif
	unsigned int dCnt;

	//KDEBUG_FUNC();
	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	if ( copy_from_user( &sParam, (void*)dArg, sizeof( sParam ) ) )
	{
		return -EFAULT;
	}
	switch ( sParam.dSize ) {
	case sizeof( unsigned char ):
		for(dCnt=0; dCnt<sParam.dDataLen; ++dCnt) {

			spin_lock_irqsave( &gpsDriver->sLock, dFlags );
			ReadRegWait( sParam.dAddress, &bData, sizeof( bData ), sParam.dWait );
			spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
			put_user( bData, (((unsigned char*)sParam.pData) + dCnt) );
		}
		break;
	case sizeof( unsigned short ):
		for(dCnt=0; dCnt<sParam.dDataLen; ++dCnt) {
			spin_lock_irqsave( &gpsDriver->sLock, dFlags);
			ReadRegWait( sParam.dAddress, &wData, sizeof( wData ), sParam.dWait );
			spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
			put_user( wData, (((unsigned short*)sParam.pData) + dCnt) );
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static inline int
IoCtl_ReadRegWait_DEBUG( int allCount ,struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		It reads it from the interface register.
		The read value is copied onto the user space.
	*/

	struct ma_IoCtlReadRegWait sParam;
	unsigned char bData = 0;
	unsigned short wData = 0;
	unsigned int dCnt;

	//KDEBUG_FUNC();

	if ( copy_from_user( &sParam, (void*)dArg, sizeof( sParam ) ) )
	{
		return -EFAULT;
	}

    if( ReadWriteCount.readCount < DEBUG_DUMP_READ_MAX ) {
        dumpRead[ReadWriteCount.readCount]=sParam;
        dump[allCount].arg=ReadWriteCount.readCount;
    }

	DK(KERN_INFO "dumpRead[%d].dAddress=%lu;\n", ReadWriteCount.readCount, sParam.dAddress); //  I/F Address
	DK(KERN_INFO "dumpRead[%d].pData=0x%p;\n", ReadWriteCount.readCount, sParam.pData); //  Read Data Store Pointer
	DK(KERN_INFO "dumpRead[%d].dSize=%u;\n", ReadWriteCount.readCount, sParam.dSize); //   Read Size(data type size)
	DK(KERN_INFO "dumpRead[%d].dDataLen=%u;\n", ReadWriteCount.readCount, sParam.dDataLen); //  Data Length
	DK(KERN_INFO "dumpRead[%d].dWait=%u;\n",ReadWriteCount.readCount ,sParam.dWait); //  Wait ns

	// 再度ReadDataをユーザ領域からカーネル領域にコピー
	switch ( sParam.dSize ) {
	case sizeof( unsigned char ):
        dumpRead[ReadWriteCount.readCount].pData = (void *)ReadWriteCount.readByteCount;
	  for(dCnt=0; dCnt<sParam.dDataLen; ++dCnt) {
	    if( ReadWriteCount.readByteCount < DEBUG_DATA_DUMP_READ_MAX ) {
	      get_user( bData, (((unsigned char*)sParam.pData) + dCnt) );
	      DK(KERN_INFO "dumpRead[%d] dumpReadByte[%d]=0x%02x;\n" ,ReadWriteCount.readCount ,ReadWriteCount.readByteCount ,bData );
              dumpReadByte[ReadWriteCount.readByteCount] = bData;
              ReadWriteCount.readByteCount++;
	    }
	  }
	  break;
	case sizeof( unsigned short ):
          dumpRead[ReadWriteCount.readCount].pData = (void *)ReadWriteCount.readWordCount;
	  for(dCnt=0; dCnt<sParam.dDataLen; ++dCnt) {
	    if( ReadWriteCount.readWordCount  < DEBUG_DATA_DUMP_READ_MAX ) {
	      get_user( wData, (((unsigned short*)sParam.pData) + dCnt) );
	      DK(KERN_INFO "dumpRead[%d] dumpReadWord[%d]=0x%04x;\n" ,ReadWriteCount.readCount ,ReadWriteCount.readWordCount ,wData );
               dumpReadWord[ReadWriteCount.readWordCount] = wData;
              ReadWriteCount.readWordCount++;
	    }
	  }
	  break;
	default:
	  return -EINVAL;
	}

	return 0;
}

/****************************************************************************
 *	DisableIrq
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
DisableIrq( void )
{
	unsigned char bStatus = 0x00;
	//KDEBUG_FUNC();
	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	// 初回割り込み?
	if ( gpsDriver->dMaskIrq == 0 )
	{
		WriteRegWait( 0x00, &bStatus, sizeof( bStatus ), 135 );
	}
	gpsDriver->dMaskIrq++;
	return 0;
}

/****************************************************************************
 *	IoCtl_DisableIrq
 *
 *	Description:
 *			The interruption is prohibited.
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	no use
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_DisableIrq( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		The interruption is prohibited.
	*/

	unsigned long dFlags;
	int sdResult = 0;
	//KDEBUG_FUNC();
	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	spin_lock_irqsave( &gpsDriver->sLock, dFlags );
	sdResult = DisableIrq();
	spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
	return sdResult;
}

/****************************************************************************
 *	EnableIrq
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
EnableIrq( void )
{
	unsigned char bStatus = 0x80;
	//KDEBUG_FUNC();
	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	if ( gpsDriver->dMaskIrq > 0UL )
	{
		gpsDriver->dMaskIrq--;
	}
	if ( gpsDriver->dMaskIrq == 0UL )
	{
		WriteRegWait( 0x00, &bStatus, sizeof( bStatus ), 135 );
	}
	return 0;
}

/****************************************************************************
 *	IoCtl_EnableIrq
 *
 *	Description:
 *			The interruption is permitted.
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	no use
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_EnableIrq( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		The interruption is permitted.
	*/

	unsigned long dFlags;
	int sdResult = 0;
	//KDEBUG_FUNC();
	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	spin_lock_irqsave( &gpsDriver->sLock, dFlags );
	sdResult = EnableIrq();
	spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
	return sdResult;
}

/****************************************************************************
 *	ResetIrqMaskCount
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
ResetIrqMaskCount( void )
{
  //KDEBUG_FUNC();
	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}

	gpsDriver->dMaskIrq = 0;
	return 0;
}

/****************************************************************************
 *	IoCtl_ResetIrqMaskCount
 *
 *	Description:
 *			The interruption mask counter is initialized.
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	no use
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_ResetIrqMaskCount( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		The interruption mask counter is initialized.
	*/

	unsigned long dFlags = 0;
	int sdResult = 0;

	(void)psInode;
	(void)psFile;
	(void)dArg;
	//KDEBUG_FUNC();

	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}

	spin_lock_irqsave( &gpsDriver->sLock, dFlags );
	sdResult = ResetIrqMaskCount();
	spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );

	return sdResult;
}

/****************************************************************************
 *	IoCtl_WaitIrq
 *
 *	Description:
 *			The interruption is waited for.
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	Pointer to variable that stores interruption frequency
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_WaitIrq( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		Sleep until interruption is notified, or waiting condition is released.
		When getting up, store the counter of interruption reserved to dArg of user space,
		and return it.
		When the interruption has already reserved it, this API call is returned at once. 
	*/

	unsigned long dFlags;
	int sdResult = 0;
	int dIrqCount = 0;

	//KDEBUG_FUNC();

	(void)psInode;
	(void)psFile;
	(void)dArg;

	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	//排他制御 (割り込み禁止
	spin_lock_irqsave( &gpsDriver->sLock, dFlags );
	// 初回判定????
	if ( gpsDriver->dIrqCount != 0 )
	{
		dIrqCount = gpsDriver->dIrqCount;
		gpsDriver->dIrqCount = 0;
		spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
	}
	else
	{
	  //割り込み復元
		spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
		for ( ;; )
		{
		  // sWait 変数宣言マクロ ( include/linux/wait.hにて定義 ) 
		  // カレントタスクの待ちキューエントリを作成する
			DEFINE_WAIT( sWait );
			// kernel/wait.cにて定義
			// 引数で渡された待ちキューに設定するなど待ちキューエントリの準備を行う
			// TASK_INTERRUPTIBLE 待機状態。シグナルによる待機状態解除可能
			prepare_to_wait( &gpsDriver->sQueue, &sWait, TASK_INTERRUPTIBLE );
			// include/linux/spinlock.h にて定義
			// #define spin_lock_irqsave(lock, flags) flags = _spin_lock_irqsave(lock)
			// スピンロックの取得を試み、取得に成功した場合、外部割り込みを無効化しEFLAGSを引数flagsへ設定する 
			spin_lock_irqsave( &gpsDriver->sLock, dFlags );

			if ( gpsDriver->dCanceled )
			{
				dIrqCount = -1;
				gpsDriver->dCanceled = 0;
				spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
				finish_wait( &gpsDriver->sQueue, &sWait );
				sdResult = -ERESTARTSYS;
				goto out;
			}

			if ( gpsDriver->dIrqCount != 0 )
			{
				dIrqCount = gpsDriver->dIrqCount;
				gpsDriver->dIrqCount = 0;
				spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
				finish_wait( &gpsDriver->sQueue, &sWait );
				sdResult = 0;
				goto out;
			}
			// スピンロック解除
			spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );
			if ( signal_pending( current ) )
			{
				dIrqCount = 0;
				finish_wait( &gpsDriver->sQueue, &sWait );
				sdResult = -ERESTARTSYS;
				goto out;
			}
			schedule();
			finish_wait( &gpsDriver->sQueue, &sWait );
		}
	}
out:
    irqFlag++;
	if ( dArg != 0 )
	{
	  // ユーザー空間へコピー   dIrqCount 値を *dArg へ書き込む
		put_user( dIrqCount, (unsigned int*)dArg );
	}
	return sdResult;
}


/****************************************************************************
 *	IoCtl_CancelWaitIrq
 *
 *	Description:
 *			The interruption waiting state is released.
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dArg	no use
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static inline int
IoCtl_CancelWaitIrq( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
	/*
		The interruption waiting state is released.
	*/
	unsigned long dFlags = 0;

	//KDEBUG_FUNC();

	(void)psInode;
	(void)psFile;
	(void)dArg;

	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}

	spin_lock_irqsave( &gpsDriver->sLock, dFlags );
	gpsDriver->dCanceled = 1;
	spin_unlock_irqrestore( &gpsDriver->sLock, dFlags );

	wake_up_interruptible( &gpsDriver->sQueue );
	return 0;
}

/****************************************************************************
 *	IrqIsMine
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
IrqIsMine( void )
{
	unsigned char bStatus = 0;
	//KDEBUG_FUNC();
	ReadRegWait( 0, &bStatus, sizeof( bStatus ), 150 );
	if ( ( bStatus & 0x01 ) == 0 || ( bStatus & 0x80 ) == 0 )
	{
		return 0;
	}
	return 1;
}

#if 1
/****************************************************************************
 *	IoCtl_SetGpio
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static inline int
IoCtl_SetGpio( struct inode *psInode, struct file *psFile, unsigned long dArg )
{
    (void)psInode;
    (void)psFile;

    if( dArg == 0 ){
        /* OFF */
        DEBUG("yamaha: ae2drv: IoCtl_SetGpio GPIO OFF");
        gpio_direction_output(121, 0);
        gpio_direction_output(27, 0);
#ifdef YAMAHA_DEBUG_LOG
        DEBUG("yamaha: ae2drv: gpio_direction_output(121, 0)\n");
        DEBUG("yamaha: ae2drv: gpio_direction_output(27, 0)\n");
#endif

    }else{
        /* ON */
        DEBUG("yamaha: ae2drv: IoCtl_SetGpio GPIO ON");
        gpio_direction_output(121, 1);
        gpio_direction_output(27, 1);
#ifdef YAMAHA_DEBUG_LOG
        DEBUG("yamaha: ae2drv: gpio_direction_output(121, 1)\n");
        DEBUG("yamaha: ae2drv: gpio_direction_output(27, 1)\n");
#endif


    }

	return 0;
}
#endif

/****************************************************************************
 *	ma_IrqHandler
 *
 *	Description:
 *			Interruption handler
 *	Arguments:
 *			sdIrq	interruption number
 *			pDevId	pointer to device ID
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
// 割り込み発生時
static irqreturn_t
ma_IrqHandler( int sdIrq, void *pDevId )
{
	/*
		It is notified that the interruption entered the interrupt processing thread.
	*/

	DEBUG();
	(void)sdIrq;

	if ( pDevId == NULL || gpsDriver == NULL )
	{
		return IRQ_NONE;
	}
	spin_lock( &gpsDriver->sLock ); /* for SMP */
	if ( !IrqIsMine() )
	{
		spin_unlock( &gpsDriver->sLock );
		return IRQ_NONE;
	}
	// 
	DisableIrq();
	gpsDriver->dIrqCount++;
	spin_unlock( &gpsDriver->sLock );
	wake_up_interruptible( &gpsDriver->sQueue );
	return IRQ_HANDLED;
}

/****************************************************************************
 *	ma_IoCtl
 *
 *	Description:
 *			Character type driver IoCtl processing is executed.
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *			dCmd	ioctl command number
 *			dArg	ioctl command argument
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static int
ma_IoCtl( struct inode *psInode, struct file *psFile, unsigned int dCmd, unsigned long dArg )
{
    int sdResult = -EFAULT;
    int count;
    //int i;
    if ( gpsDriver == NULL ){
        return -ENOTTY;
    }
    count= ReadWriteCount.allCount++;
    if( dump !=NULL && count >= DEBUG_DUMP_START && count <= DEBUG_DUMP_END ) {
        dump[count].cmd=dCmd;
        dump[count].arg=dArg;
        if( (count % PERIOD ==0) || (count>=3400 && count <=3500) || (dCmd ==  MA_IOCTL_SET_GPIO) ){
            DEBUG("dump[%d].cmd=%08x;", count, dCmd );
            if(count % PERIOD ==0){
                gpio_get_value(27);
                //gpio_get_value(28);
                gpio_get_value(102);
                gpio_get_value(121);
            }
        }
    }


    switch ( dCmd ) {
    case MA_IOCTL_WAIT://0
        //DI("WAIT0\n" );
        ReadWriteCount.waitCount++;
        sdResult = IoCtl_Wait( psInode, psFile, dArg );
        break;
    case MA_IOCTL_SLEEP://1
        //DI("SLEEP1\n" );
        ReadWriteCount.sleepCount++;
        sdResult = IoCtl_Sleep( psInode, psFile, dArg );
        break;
    case MA_IOCTL_WRITE_REG_WAIT://2
        if( count >= DEBUG_DUMP_START && count <= DEBUG_DUMP_END  ) {
            DI("//WRITE\n");
            IoCtl_WriteRegWait_DEBUG( count , psInode, psFile, dArg );
        }
        ReadWriteCount.writeCount++;
        sdResult = IoCtl_WriteRegWait( psInode, psFile, dArg );
        break;
        
    case MA_IOCTL_READ_REG_WAIT://3
        sdResult = IoCtl_ReadRegWait( psInode, psFile, dArg );
        if( count >= DEBUG_DUMP_START && count <= DEBUG_DUMP_END  ) {
            DI("//READ\n");
            IoCtl_ReadRegWait_DEBUG( count , psInode, psFile, dArg );
        }
        ReadWriteCount.readCount++;
		break;
    case MA_IOCTL_DISABLE_IRQ://4
        //DI("xIRQ4\n" );
        ReadWriteCount.disableCount++;
        sdResult = IoCtl_DisableIrq( psInode, psFile, dArg );
        break;
    case MA_IOCTL_ENABLE_IRQ://5
        //DI("oIRQ5\n" );
        ReadWriteCount.enableCount++;
        sdResult = IoCtl_EnableIrq( psInode, psFile, dArg );
        break;
    case MA_IOCTL_RESET_IRQ_MASK_COUNT://6
        DI("RESET_IRQ_MASK_COUNT6 dCmd=0x%08x dArd=%lu dArg=0x%lx\n",dCmd , dArg , dArg );
        ReadWriteCount.resetCount++;
        sdResult = IoCtl_ResetIrqMaskCount( psInode, psFile, dArg );
        break;

    case MA_IOCTL_WAIT_IRQ://7
        DI("WAIT_IRQ dCmd=0x%08x dArd=%lu dArg=0x%lx\n",dCmd , dArg , dArg );
        ReadWriteCount.waitIRQCount++;
        sdResult = IoCtl_WaitIrq( psInode, psFile, dArg );
        break;

    case MA_IOCTL_CANCEL_WAIT_IRQ://8
        DI("CANCEL_WAIT_IRQ8 dCmd=0x%08x dArd=%lu dArg=0x%lx\n",dCmd , dArg , dArg );
        ReadWriteCount.cancelCount++;
        sdResult = IoCtl_CancelWaitIrq( psInode, psFile, dArg );
        break;

    case MA_IOCTL_SET_GPIO://9
        DI("SET_GPIO9 dCmd=0x%08x dArd=%lu\n",dCmd , dArg );
        ReadWriteCount.setCount++;
        sdResult = IoCtl_SetGpio( psInode, psFile, dArg );
        break;
        
    default:
        DI("default dCmd=0x%08x dArd=%lu dArg=0x%lx\n",dCmd , dArg , dArg );
        sdResult = -ENOTTY;
        break;
    }
	return sdResult;
}

/****************************************************************************
 * リード対応
 *
 * 引数
 *  psFile	file info pointer
 *
 * 戻り値 
 *  終了: 0
 *  成功：転送バイト数
 *  エラー：-EFAULT
 ****************************************************************************/
static ssize_t 
ma_ReadDebug( struct file* psFile, char* buf, size_t count, loff_t* pos )
{
    int textLen,copyLen;
    char text[1024],textBuf[1024];
 
    if( dump == NULL ) {
      D("failure dump=NULL readCount=%d allCount=%d\n",  readCount, ReadWriteCount.allCount );
      readCount=0;
      return 0;
    }

    // 開始
    if( readCount== 0 ){
      D("start request_count=%d, readCount=%d allCount=%d\n", count, readCount, ReadWriteCount.allCount );
    }

    // 終了
    if( readCount >= ReadWriteCount.allCount || readCount >=  DEBUG_DUMP_END ) {
      D("complete request_count=%d, readCount=%d allCount=%d\n", count, readCount, ReadWriteCount.allCount  );
      readCount=0;
      return 0;
    }

    // ioctl log 整形出力
    if( readDataMode == 0 ) {
        snprintf(text,1024,"dump[%d].cmd=0x%08x;//%s\n",readCount, dump[readCount].cmd, cmdToText[_IOC_NR(dump[readCount].cmd)] );
    }

    switch ( dump[readCount].cmd ) {
    case MA_IOCTL_WAIT://0
        snprintf(textBuf,1024,"%sdump[%d].arg=%u;\n",text,readCount, dump[readCount].arg );
        textBuf[sizeof(textBuf)-1]='\0';
        strcpy(text,textBuf);        
    break;

    case MA_IOCTL_SLEEP://1
        snprintf(textBuf,1024,"%sdump[%d].arg=%u;\n",text,readCount, dump[readCount].arg );
        textBuf[sizeof(textBuf)-1]='\0';
        strcpy(text,textBuf);
     break;

    case MA_IOCTL_WRITE_REG_WAIT://2
        if( readDataMode == 0 ) {
            snprintf(textBuf,1024,
                     "%s"
                     "dump[%d].arg=%d;\n"
                     "dumpWrite[%d].dAddress=%lu;\n"
                     "dumpWrite[%d].pData=&%s[%u];\n"
                     "dumpWrite[%d].dSize=%u;\n"
                     "dumpWrite[%d].dDataLen=%u;\n"
                     "dumpWrite[%d].dWait=%u;\n",
                     text,
                     readCount, dump[readCount].arg,
                     dump[readCount].arg, dumpWrite[dump[readCount].arg].dAddress,      // I/F Address
                     dump[readCount].arg, 
                     (dumpWrite[dump[readCount].arg].dSize==sizeof( unsigned char ))?"dumpWriteByte":"dumpWriteWord",
                     (unsigned int)dumpWrite[dump[readCount].arg].pData,          // Write Pointer 
                     dump[readCount].arg, dumpWrite[dump[readCount].arg].dSize,          // Write Size(data type size)
                     dump[readCount].arg, dumpWrite[dump[readCount].arg].dDataLen,       // Data Length
                     dump[readCount].arg, dumpWrite[dump[readCount].arg].dWait );        // Wait ns
            textBuf[sizeof(textBuf)-1]='\0';
            strcpy(text,textBuf);
            readDataCount = (unsigned int)dumpWrite[dump[readCount].arg].pData;
            readDataMode=1;
        } else {
            switch( dumpWrite[dump[readCount].arg].dSize ) {
            case sizeof( unsigned char ):
                snprintf(textBuf,1024,"dumpWriteByte[%d]=0x%02x;\n", readDataCount, dumpWriteByte[readDataCount] );
                textBuf[sizeof(textBuf)-1]='\0';
                strcpy(text,textBuf);
                readDataCount++;
                if( readDataCount == (unsigned int)dumpWrite[dump[readCount].arg].pData+dumpWrite[dump[readCount].arg].dDataLen ) {
                    readDataMode = 0;
                }
                break;

            case sizeof( unsigned short ):
                snprintf(textBuf,1024,"dumpWriteWord[%d]=0x%04x;\n",readDataCount,dumpWriteWord[readDataCount]);
                textBuf[sizeof(textBuf)-1]='\0';
                strcpy(text,textBuf);
                readDataCount++;
                if( readDataCount == (unsigned int)dumpWrite[dump[readCount].arg].pData+dumpWrite[dump[readCount].arg].dDataLen ) {
                    readDataMode = 0;
                }
                break;
            }
        }
        break;

    case MA_IOCTL_READ_REG_WAIT://3
        if( readDataMode == 0 ) {
            snprintf(textBuf,1024,
                     "%s"
                     "dump[%d].arg=%d;\n"
                     "dumpRead[%d].dAddress=%lu;\n"
                     "dumpRead[%d].pData=&%s[%u];\n"
                     "dumpRead[%d].dSize=%u;\n"
                     "dumpRead[%d].dDataLen=%u;\n"
                     "dumpRead[%d].dWait=%u;\n",
                     text,
                     readCount, dump[readCount].arg,
                     dump[readCount].arg, dumpRead[dump[readCount].arg].dAddress,      // I/F Address
                     dump[readCount].arg,
                     (dumpRead[dump[readCount].arg].dSize==sizeof( unsigned char ))?"dumpReadByte":"dumpReadWord",
                     (unsigned int)dumpRead[dump[readCount].arg].pData,          // Read Pointer 
                     dump[readCount].arg, dumpRead[dump[readCount].arg].dSize,          // Read Size(data type size)
                     dump[readCount].arg, dumpRead[dump[readCount].arg].dDataLen,       // Data Length
                     dump[readCount].arg, dumpRead[dump[readCount].arg].dWait );        // Wait ns
            textBuf[sizeof(textBuf)-1]='\0';
            strcpy(text,textBuf);
            readDataCount = (unsigned int)dumpRead[dump[readCount].arg].pData;
            readDataMode=1;
        } else {
            switch( dumpRead[dump[readCount].arg].dSize ) {
            case sizeof( unsigned char ):
                snprintf(textBuf,1024,"dumpReadByte[%d]=0x%02x;\n", readDataCount, dumpReadByte[readDataCount] );
                textBuf[sizeof(textBuf)-1]='\0';
                strcpy(text,textBuf);
                readDataCount++;
                if( readDataCount == (unsigned int)dumpRead[dump[readCount].arg].pData+dumpRead[dump[readCount].arg].dDataLen ) {
                    readDataMode = 0;
                }
                break;

            case sizeof( unsigned short ):
                snprintf(textBuf,1024,"dumpReadWord[%d]=0x%04x;\n",readDataCount,dumpReadWord[readDataCount]);
                textBuf[sizeof(textBuf)-1]='\0';
                strcpy(text,textBuf);
                readDataCount++;
                if( readDataCount == (unsigned int)dumpRead[dump[readCount].arg].pData+dumpRead[dump[readCount].arg].dDataLen ) {
                    readDataMode = 0;
                }
                break;
            }
        }
      break;

    case MA_IOCTL_DISABLE_IRQ://4
      break;
    case MA_IOCTL_ENABLE_IRQ://5
      break;
    case MA_IOCTL_RESET_IRQ_MASK_COUNT://6
      break;
    case MA_IOCTL_WAIT_IRQ://7
        /*snprintf(textBuf,1024,"%sdump[%d].arg=%u;\n",text,readCount, dump[readCount].arg );
        textBuf[sizeof(textBuf)-1]='\0';
        strcpy(text,textBuf);*/
      break;
    case MA_IOCTL_CANCEL_WAIT_IRQ://8
      break;
    case MA_IOCTL_SET_GPIO://9
        snprintf(textBuf,1024,"%sdump[%d].arg=%u;\n",text,readCount, dump[readCount].arg );
        textBuf[sizeof(textBuf)-1]='\0';
        strcpy(text,textBuf);
      break;

    default:
        snprintf(textBuf,1024,"%s!!!!!!!!!!!!!!! ERROR !!!!!!!!!!!!!!!!!!!!\n",text);
        textBuf[sizeof(textBuf)-1]='\0';
        strcpy(text,textBuf);
      break;
    }

    // 合計データ出力
    if( readDataMode ==0 && (readCount >= (ReadWriteCount.allCount-1) || readCount >=  (DEBUG_DUMP_END-1)) ) {
        snprintf(textBuf,1024,
                 "%s\n"
                 "ReadWriteCount.allCount=%d;\n"
                 "ReadWriteCount.waitCount=%d;\n"
                 "ReadWriteCount.sleepCount=%d;\n"
                 "ReadWriteCount.writeCount=%d;\n"
                 "ReadWriteCount.writeByteCount=%d;\n"
                 "ReadWriteCount.writeWordCount=%d;\n"
                 "ReadWriteCount.readCount=%d;\n"
                 "ReadWriteCount.readByteCount=%d;\n"
                 "ReadWriteCount.readWordCount=%d;\n"
                 "ReadWriteCount.disableCount=%d;\n"
                 "ReadWriteCount.enableCount=%d;\n"
                 "ReadWriteCount.resetCount=%d;\n"
                 "ReadWriteCount.waitIRQCount=%d;\n"
                 "ReadWriteCount.cancelCount=%d;\n"
                 "ReadWriteCount.setCount=%d;\n"
                 "\n//irqFlag=%d;\n",
                 text,
                 ReadWriteCount.allCount,
                 ReadWriteCount.waitCount,
                 ReadWriteCount.sleepCount,
                 ReadWriteCount.writeCount,
                 ReadWriteCount.writeByteCount,
                 ReadWriteCount.writeWordCount,
                 ReadWriteCount.readCount,
                 ReadWriteCount.readByteCount,
                 ReadWriteCount.readWordCount,
                 ReadWriteCount.disableCount,
                 ReadWriteCount.enableCount,
                 ReadWriteCount.resetCount,
                 ReadWriteCount.waitIRQCount,
                 ReadWriteCount.cancelCount,
                 ReadWriteCount.setCount,
                 irqFlag );
        textBuf[sizeof(textBuf)-1]='\0';
        strcpy(text,textBuf);
    }

    text[sizeof(text)-1]='\0';
    textLen = strlen( text );
    if ( count > textLen ){
        copyLen = textLen;
    } else {
        copyLen = count;
        D("read count size over\n");
    }
    if ( copy_to_user( buf, text, copyLen ) ) {
        D( "copy_to_user failed\n" );
        return -EFAULT;
    }

    *pos += copyLen;
    if( readDataMode == 0 ) {
        readCount++;
    }

    return copyLen;
}
// ファイル オープン対応
static int ma_OpenDebug( struct inode *inode, struct file *filp ) 
{
    DEBUG();
    readCount=0;
    return 0;
}


// ファイル クローズ対応
static int ma_CloseDebug( struct inode* inode, struct file* filp )
{
    DEBUG();
    return 0;
}


/****************************************************************************
 *	ma_Open
 *
 *	Description:
 *	Arguments:
 *	Return:
 *
 ****************************************************************************/
static int
ma_Open( struct inode *psInode, struct file *psFile )
{
	/*
		Character type driver Open processes it.
	*/
	int sdResult = 0;

	DEBUG();

	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}

#if 1
    // 121,27 は ioctl でON,OFF される
	gpio_tlmm_config(GPIO_CFG(27,  0, GPIO_OUTPUT, GPIO_NO_PULL,   GPIO_2MA), GPIO_ENABLE);
	gpio_tlmm_config(GPIO_CFG(28,  0, GPIO_INPUT,  GPIO_PULL_DOWN, GPIO_2MA), GPIO_ENABLE);
	gpio_tlmm_config(GPIO_CFG(102, 1, GPIO_OUTPUT, GPIO_NO_PULL,   GPIO_2MA), GPIO_ENABLE);
	gpio_tlmm_config(GPIO_CFG(121, 0, GPIO_OUTPUT, GPIO_NO_PULL,   GPIO_2MA), GPIO_ENABLE);

	DEBUG("yamaha: ae2drv: ma_Open GPIO ON");
	gpio_direction_output(121, 1);
	gpio_direction_output(27, 1);
#ifdef YAMAHA_DEBUG_LOG
#endif
#endif

	/* init */
	init_waitqueue_head( &gpsDriver->sQueue );
	//スピンロックを初期化（ロック解放状態）
	//include/asm-i386/spinlock.hにて定義(x86の場合)
	//SPIN_LOCK_UNLOCKEDは1と定義
	spin_lock_init( &gpsDriver->sLock );
	gpsDriver->dIrqCount = 0;
	gpsDriver->dCanceled = 0;

	/* I/O port setting */
#if 0
	gpsDriver->pMemory = ioremap( ?, 64 );
#else
	//物理アドレスをカーネル空間へリマップ??
	gpsDriver->pMemory = ioremap( 0x90000000, 64 );
#endif
	if ( gpsDriver->pMemory == NULL )
	{
		goto err1;
	}

	/* interrrupt setting */ //割り込み設定(上行にて定義) gpsDriver->dMaskIrq = 0;
	ResetIrqMaskCount();

#if 0
	gpsDriver->dIrq = ?;
	sdResult = request_irq( gpsDriver->dIrq, ma_IrqHandler, IRQF_SHARED, MA_DEVICE_IRQ_NAME, gpsDriver );
	if ( sdResult < 0 )
	{
		goto err2;
	}
	set_irq_type( gpsDriver->dIrq, IRQT_FALLING );
#else
	//IRQ 数値設定
	gpsDriver->dIrq = MSM_GPIO_TO_INT(28);
	//IRQ 登録
	// gpsDriver->dIrq 登録を要求する割り込みirq番号
	// ma_IrqHandler ハンドラ関数へのポインタ
	// IRQF_TRIGGER_FALLING 立ち下がりタイミング
	// MA_DEVICE_NAME 割り込みの所有を表す文字列、表示に利用される
	// gpsDriver デバイス識別子  割り込み関数が呼び出されるときの第2引数として渡される
	sdResult = request_irq( gpsDriver->dIrq, ma_IrqHandler, IRQF_TRIGGER_FALLING, MA_DEVICE_NAME, gpsDriver );
	if ( sdResult < 0 )
	{
		goto err2;
	}
#endif

	return 0;
err2:
	iounmap( gpsDriver->pMemory );
	gpsDriver->pMemory = NULL;
err1:
#if 1
	gpio_direction_output(121, 0);
	gpio_direction_output(27, 0);
#ifdef YAMAHA_DEBUG_LOG
	DK("yamaha: ae2drv: ma_Open GPIO Err OFF\n");
#endif
#endif

	return sdResult;
}


/****************************************************************************
 *	ma_Close
 *
 *	Description:
 *			Character type driver Close processing is executed.
 *	Arguments:
 *			psInode	inode info pointer
 *			psFile	file info pointer
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static int
ma_Close( struct inode *psInode, struct file *psFile )
{
  //  int i;
	/*
		Character type driver Close processes it.
	*/
	DEBUG();

	(void)psInode;
	(void)psFile;

	if ( gpsDriver == NULL )
	{
		return -ENOTTY;
	}
	if ( gpsDriver->pMemory == NULL )
	{
		return -ENOTTY;
	}

	free_irq( gpsDriver->dIrq, gpsDriver );
	iounmap( gpsDriver->pMemory );
	gpsDriver->pMemory = NULL;

#if 1
	gpio_direction_output(27, 0);
	gpio_direction_output(121, 0);
#endif
	return 0;
}

/****************************************************************************
 *	ma_Init
 *
 *	Description:
 *			The driver is initialized.
 *	Arguments:
 *			none
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static int __init
ma_Init( void )
{
    /*
      Processing that registers the character type driver is done.
    */

    int sdResult,sdResultDebug, major;
	struct device *dev;
	dev_t devNum;

	DEBUG();
	//キャラクターデバイス番号の動的割り当て
	//int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name)
	//  dev_t *dev              保存先 割り当て結果dev_t変数ポインタ
	//  unsigned int baseminor  マイナー番号の開始番号
	//  unsigned int count      割り当てるマイナー番号の数
	//  const char *name        デバイス名
	//sdResult = alloc_chrdev_region( &gsDev, 0, MA_DEVICE_COUNT, MA_DEVICE_NAME );
	sdResult = alloc_chrdev_region( &gsDev, 0, 2, MA_DEVICE_NAME );
	if ( sdResult < 0 )
	{
		goto err1;
	}
	// メジャー番号
	gsMajor = sdResult;
	major = MAJOR(gsDev);
	D("gsMajor=%d , MAJOR()=%d , MINOR()=%d\n", gsMajor, MAJOR(gsDev), MINOR(gsDev) );

	// ドライバ用構造体メモリ確保
	gpsDriver = kzalloc( sizeof( struct MaDriverInfo ), GFP_KERNEL );
	if ( gpsDriver == NULL )
	{
		sdResult = -ENOMEM;
		goto err2;
	}

	// キャラクタデバイス初期化
	//void cdev_init(struct cdev *cdev, struct file_operations *fops)
	//  struct cdev *cdev       保存先 キャラクタデバイス構造体ポインタ
	//  struct file_operations *fops  ファイルオペレーションズ構造体
	cdev_init( &gpsDriver->sCdev, &ma_FileOps );
	gpsDriver->sCdev.owner = THIS_MODULE;
	gpsDriver->sCdev.ops = &ma_FileOps;
	// デバッグ用キャラクタデバイス初期化
	cdev_init( &sCdevDebug, &ma_FileOpsDebug );
	sCdevDebug.owner = THIS_MODULE;
	//sCdevDebug.ops = &ma_FileOpsDebug;

	// キャラクタデバイスの登録
	//int cdev_add(struct cdev *p, dev_t dev, unsigned count)
	//  struct cdev *p          登録するキャラクタデバイス構造体
	//  dev_t dev               デバイス
	//  unsigned count          数
	//sdResult = cdev_add( &gpsDriver->sCdev, gsDev, MA_DEVICE_COUNT );
	devNum = MKDEV(major, 0);
	sdResult = cdev_add( &gpsDriver->sCdev, devNum, 1 );
	if ( sdResult < 0 )
	{
		goto err3;
	}
	// デバッグ用キャラクタデバイス初期化
	devNum = MKDEV(major, 1);
	sdResultDebug = cdev_add( &sCdevDebug, devNum, 1 );
	if ( sdResultDebug < 0 )
	{
		goto err35;
	}



	// class_create - create a struct class structure 
	//struct class * class_create(struct module  * owner, const char * name); 
	//Arguments
	// owner:  pointer to the module that is to “own” this struct class
	// name:   pointer to a string for the name of this class. 
	gpsClass = class_create( THIS_MODULE, MA_CLASS_NAME );
	if ( IS_ERR(gpsClass) )
	{
		sdResult = PTR_ERR( gpsClass );
		goto err4;
	}

#if 1
	// device_create - creates a device and registers it with sysfs 
	//struct device * device_create(struct class  * class, struct device * parent, dev_t devt, const char * fmt, ...);
	// class   pointer to the struct class that this device should be registered to
	// parent  pointer to the parent struct device of this new device, if any
	// devt    the dev_t for the char device to be added
	// fmt     string for the device´s name
	// ...     variable arguments 
	dev = device_create( gpsClass, NULL, gsDev, NULL, MA_DEVICE_NODE_NAME );
	// 要エラー処理
	device_create( gpsClass, NULL, devNum, NULL, MA_DEVICE_NODE_NAME "debug" );
#else
	dev = device_create( gpsClass, NULL, gsDev, MA_DEVICE_NODE_NAME );
#endif
	sdResult = IS_ERR(dev) ? PTR_ERR(dev) : 0;
	if ( sdResult < 0 )
	{
		goto err5;
	}

dump = vmalloc( DEBUG_DUMP_END*sizeof(struct cmd_dump) );
 if( dump !=NULL )
     D("dump=vmalloc(%dKB) OK\n", DEBUG_DUMP_END*sizeof(struct cmd_dump)/1024);
 else
   D("dump=vmalloc() NG\n");

dumpRead = vmalloc( DEBUG_DUMP_READ_MAX * sizeof(struct ma_IoCtlReadRegWait) );
 if( dumpRead !=NULL )
     D("dumpRead=vmalloc(%dKB) OK\n", DEBUG_DUMP_READ_MAX * sizeof(struct ma_IoCtlReadRegWait)/1024);
 else
   D("dumpRead=vmalloc() NG\n");

dumpWrite = vmalloc(  DEBUG_DUMP_WRITE_MAX * sizeof(struct ma_IoCtlWriteRegWait) );
if( dumpWrite !=NULL )
    D("dumpWrite=vmalloc(%dKB) OK\n", DEBUG_DUMP_WRITE_MAX * sizeof(struct ma_IoCtlWriteRegWait) /1024);
else
    D("dumpWrite=vmalloc() NG\n");

dumpWriteByte = vmalloc( DEBUG_DATA_DUMP_WRITE_MAX *sizeof(unsigned char) );
if( dumpWriteByte  !=NULL )
    D("dumpWriteByte =vmalloc(%dKB) OK\n", DEBUG_DATA_DUMP_WRITE_MAX *sizeof(unsigned char) /1024);
else
    D("dumpWriteByte =vmalloc() NG\n");
    
dumpWriteWord = vmalloc( DEBUG_DATA_DUMP_WRITE_MAX *sizeof(unsigned short) );
if( dumpWriteWord  !=NULL )
    D("dumpWriteWord =vmalloc(%dKB) OK\n", DEBUG_DATA_DUMP_WRITE_MAX *sizeof(unsigned short)/1024);
else
    D("dumpWriteWord =vmalloc() NG\n");

dumpReadByte = vmalloc( DEBUG_DATA_DUMP_READ_MAX *sizeof(unsigned char) );
if( dumpReadByte  !=NULL )
    D("dumpReadByte =vmalloc(%dKB) OK\n",  DEBUG_DATA_DUMP_READ_MAX *sizeof(unsigned char)/1024);
else
    D("dumpReadByte =vmalloc() NG\n");
    
dumpReadWord = vmalloc( DEBUG_DATA_DUMP_READ_MAX *sizeof(unsigned short) );
if( dumpReadWord  !=NULL )
    D("dumpReadWord =vmalloc(%dKB) OK\n",  DEBUG_DATA_DUMP_READ_MAX *sizeof(unsigned short)/1024);
else
    D("dumpReadWord =vmalloc() NG\n");

ReadWriteCount.allCount=0;
ReadWriteCount.waitCount=0;
ReadWriteCount.sleepCount=0;
ReadWriteCount.writeCount=0;
ReadWriteCount.writeByteCount=0;
ReadWriteCount.writeWordCount=0;
ReadWriteCount.readCount=0;
ReadWriteCount.readByteCount=0;
ReadWriteCount.readWordCount=0;
ReadWriteCount.disableCount=0;
ReadWriteCount.enableCount=0;
ReadWriteCount.resetCount=0;
ReadWriteCount.waitIRQCount=0;
ReadWriteCount.cancelCount=0;
ReadWriteCount.setCount=0;

	return 0;

err5:
	class_destroy( gpsClass );
	gpsClass = NULL;
err4:
	cdev_del( &gpsDriver->sCdev );
err35:
	cdev_del( &sCdevDebug );
err3:
	kfree( gpsDriver );
	gpsDriver = NULL;
err2:
	unregister_chrdev_region( gsDev, 2 );
	gsMajor = -1;
err1:
	return sdResult;
}

/****************************************************************************
 *	ma_Term
 *
 *	Description:
 *			The driver is ended.
 *	Arguments:
 *			none
 *	Return:
 *			0		success
 *			< 0		error code
 *
 ****************************************************************************/
static void __exit
ma_Term( void )
{
	/*
		Processing that deletes the character type driver is done.
	*/

	//KDEBUG_FUNC();
    DEBUG();

	if ( gsMajor < 0 )
	{
		return;
	}

	device_destroy( gpsClass, MKDEV(MAJOR(gsDev),1) );
	device_destroy( gpsClass, gsDev );

	class_destroy( gpsClass );
	gpsClass = NULL;

	cdev_del( &gpsDriver->sCdev );
	cdev_del( &sCdevDebug );
	kfree( gpsDriver );
	gpsDriver = NULL;

	unregister_chrdev_region( gsDev, 2 );
	gsMajor = -1;

D("ReadWriteCount.allCount=%d\n", ReadWriteCount.allCount );
if( dump !=NULL ){
    vfree(dump);
    D("vfree(dump)\n");
}

if( dumpRead !=NULL ){
    vfree(dumpRead);
    D("vfree(dumpRead)\n");
 }

if( dumpWrite !=NULL ) {
    vfree(dumpWrite);
    D("vfree(dumpWrite)\n");
}

if( dumpWriteByte != NULL ) {
    vfree(dumpWriteByte);
    D("vfree(dumpWriteByte)\n");
 }

if( dumpWriteWord != NULL ) {
    vfree(dumpWriteWord);
    D("vfree(dumpWriteWord)\n");
 }

if( dumpReadByte != NULL ) {
    vfree(dumpReadByte);
    D("vfree(dumpReadByte)\n");
 }

if( dumpReadWord != NULL ) {
    vfree(dumpReadWord);
    D("vfree(dumpReadWord)\n");
 }

	return;
}

module_init( ma_Init );
module_exit( ma_Term );

MODULE_DESCRIPTION("ae2 driver");
MODULE_LICENSE("GPL");
