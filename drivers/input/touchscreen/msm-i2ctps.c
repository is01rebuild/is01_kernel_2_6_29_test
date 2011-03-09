/*
 * Copyright (C) 2009 SHARP CORPORATION All rights reserved.
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

#define	TPS_ROTATE_180			/* 座標の180度回転 */
#define	TPS_PRNERR				/* ログ出力(エラーログ) */
/* #define	TPS_PRNLOG */				/* ログ出力(通常ログ) */
/* #define	TPS_PRNDEB */				/* ログ出力(デバッグログ) */

/*+-------------------------------------------------------------------------+*/
/*|	インクルードファイル													|*/
/*+-------------------------------------------------------------------------+*/
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/cdev.h>
#include <mach/gpio.h>
#include <mach/msm_i2ctps.h>
#include <sharp/shlcdc_kerl.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>

#include <mach/msm_i2ctps_fw.h>

/*+-------------------------------------------------------------------------+*/
/*|	定数宣言																|*/
/*+-------------------------------------------------------------------------+*/

#define	TPS_I2C_RETRY			10

#define	KPD_KEYPRESS			1
#define	KPD_KEYRELEASE			0

/* コマンド */
enum
{
	QPHYSLEN			= 128,
	QCVENDOR_ID			= 0x5143,
	QCPRODUCT_ID		= 2,
	QCVERSION_ID		= 1
};

#define	INITDELAY_TIMER			600
#define	RECOVER_TIMER			1000

typedef enum
{
	TPS_STATE_HOVER = 0,
	TPS_STATE_SDOWN,
	TPS_STATE_MDOWN,
	TPS_STATE_WIDE,
	TPS_STATE_3DOWN,
	TPS_STATE_MAX,
} TpsState;

#define	TPS_STATE_ZM		0x80
#define	TPS_STATE_MT		0x20
#define	TPS_STATE_ST		0x10
#define	TPS_STATE_DR		0x08
#define	TPS_STATE_FL		0x04
#define	TPS_STATE_DC		0x02
#define	TPS_STATE_SC		0x01

#define	TPS_ERROR_VALUE		0xfff

/* 広範囲・３点押しエラー時に通知する値 */
#define TPS_ERROR_POS_X		240
#define	TPS_ERROR_POS_Y		480
#define	TPS_ERROR_WIDE		SH_TOUCH_MAX_DISTANCE	/* 広範囲 */
#define	TPS_ERROR_3DOWN		SH_TOUCH_MAX_DISTANCE	/* ３点押し */

/* 調整パラメータ */
#define	POS_X0				0
#define	POS_X1				120
#define	POS_X2				360
#define	POS_X3				480
#define	POS_Y0				0
#define	POS_Y1				180
#define	POS_Y2				480
#define	POS_Y3				780
#define	POS_Y4				960
#define	POS_LIMIT			100

#define	ADJUST_POINT		6					/* 補正ポイント数 */
#define	AREA_COUNT			(ADJUST_POINT*2)	/* 補正エリア数 */
#define DOUBLE_ACCURACY		10000

#define	TPS_DISABLE_FLIP	0x01
#define	TPS_DISABLE_SLEEP	0x02
#define	TPS_DISABLE_API		0x04

#define	TPS_DISABLE_ON		0xFF
#define	TPS_DISABLE_OFF		0x00

#define	TPS_CHECK_ON		0x01
#define	TPS_CHECK_OFF		0x00

#define	TPS_RETURN_ON		0x01
#define	TPS_RETURN_OFF		0x00

/*+-------------------------------------------------------------------------+*/
/*|	型宣言																	|*/
/*+-------------------------------------------------------------------------+*/
typedef struct i2ctps_record	I2cTpsRec;
typedef struct i2c_client		I2cClient;
typedef struct i2c_device_id	I2cDevID;
typedef struct work_struct		WorkStruct;
typedef struct input_dev		InputDev;
typedef struct device			Device;

struct i2ctps_record
{
	I2cClient *mpoClient;
	InputDev *mpoInDev;
	int		mnProductInfo;
	char	mcPhysInfo[QPHYSLEN];
	int		mnIrqPin;
	int		(*mpfPinSetupFunc)(void);
	void	(*mpfPinShutdownFunc)(void);
	uint8_t	mbIsActive;					/* ドライバステータス(1:動作中/0:停止中) */
	struct delayed_work moCmdQ;
	WorkStruct moIrqWork;
	int		mnHsspClkPin;
	int		mnHsspDataPin;
	TpsState mnState;
	uint8_t	mbIsEnable;					/* ステータス(1:動作中/0:停止中) */
	uint8_t	mbIsTestMode;				/* TestModeステータス(1:テストモード/0:通常モード) */
	uint8_t	mbAdjustEnable;				/* 補正(1:有効 0:無効) */
	uint8_t	mbAccessState;				/* アクセス状態 */
	uint8_t	mbIsFirst;					/* 初回起動時 */
};

typedef struct
{
	short x;
	short y;
} TpsPoint_t;

typedef struct
{
	TpsPoint_t p;		/* 左上 */
	TpsPoint_t q;		/* 右上 */
	TpsPoint_t r;		/* 左下 */
	TpsPoint_t s;		/* 右下 */
} TpsArea_t;

typedef struct
{
	short mValue;
	short mNo;
} Qsort_t;

/* ディスパッチテーブル定義型 */
typedef struct
{
	uint8_t mbValid;;							/* 0なら無効 */
												/* イベント処理関数 */
	void (*mpReportKey)(InputDev *);
	void (*mpReportWit)(InputDev *, uint16_t, uint16_t);
	void (*mpReportPos)(InputDev *, uint16_t, uint16_t);
} TpsDispatch;


static struct semaphore sem;

/*+-------------------------------------------------------------------------+*/
/*|	広域変数の定義															|*/
/*+-------------------------------------------------------------------------+*/
static uint8_t gbSetParam[7] = 
	/*	0x09  0x0A  0x0B  0x0C  0x0D  0x0E  0x0F	*/
	{	0xFF, 0xFF, 0x00, 0xFF, 0x05, 0xFF, 0x05	};

/* 補正基準となる６点の座標 */
static const TpsPoint_t gBasePt[ADJUST_POINT] =
		{
			{POS_X1, POS_Y1}, {POS_X2, POS_Y1},
			{POS_X1, POS_Y2}, {POS_X2, POS_Y2},
			{POS_X1, POS_Y3}, {POS_X2, POS_Y3},
		};
/* 補正分割エリアの座標 */
static const TpsArea_t gAreaRect[AREA_COUNT] =
		{
			{{POS_X0, POS_Y0}, {POS_X1, POS_Y0}, {POS_X0, POS_Y1}, {POS_X1, POS_Y1}},
			{{POS_X1, POS_Y0}, {POS_X2, POS_Y0}, {POS_X1, POS_Y1}, {POS_X2, POS_Y1}},
			{{POS_X2, POS_Y0}, {POS_X3, POS_Y0}, {POS_X2, POS_Y1}, {POS_X3, POS_Y1}},
			{{POS_X0, POS_Y1}, {POS_X1, POS_Y1}, {POS_X0, POS_Y2}, {POS_X1, POS_Y2}},
			{{POS_X1, POS_Y1}, {POS_X2, POS_Y1}, {POS_X1, POS_Y2}, {POS_X2, POS_Y2}},
			{{POS_X2, POS_Y1}, {POS_X3, POS_Y1}, {POS_X2, POS_Y2}, {POS_X3, POS_Y2}},
			{{POS_X0, POS_Y2}, {POS_X1, POS_Y2}, {POS_X0, POS_Y3}, {POS_X1, POS_Y3}},
			{{POS_X1, POS_Y2}, {POS_X2, POS_Y2}, {POS_X1, POS_Y3}, {POS_X2, POS_Y3}},
			{{POS_X2, POS_Y2}, {POS_X3, POS_Y2}, {POS_X2, POS_Y3}, {POS_X3, POS_Y3}},
			{{POS_X0, POS_Y3}, {POS_X1, POS_Y3}, {POS_X0, POS_Y4}, {POS_X1, POS_Y4}},
			{{POS_X1, POS_Y3}, {POS_X2, POS_Y3}, {POS_X1, POS_Y4}, {POS_X2, POS_Y4}},
			{{POS_X2, POS_Y3}, {POS_X3, POS_Y3}, {POS_X2, POS_Y4}, {POS_X3, POS_Y4}},

		};
static TpsArea_t gAreaDiff[AREA_COUNT];
static TpsPoint_t gAdjustPrm[ADJUST_POINT];
static uint8_t gSense[168];

static int gnResult = 0;

/*+-------------------------------------------------------------------------+*/
/*|	プロトタイプ宣言														|*/
/*+-------------------------------------------------------------------------+*/
/* I2Cアクセス */
static int ShTps_I2cRead(I2cClient *poClient, uint8_t bRegAdr, uint8_t *pbBuf, uint32_t dwLen);
static int ShTps_I2cWriteOne(I2cClient *poClient, uint8_t bRegAdr, uint8_t bData);
static int ShTps_I2cWrite(I2cClient *poClient, uint8_t bRegAdr, uint8_t *pbBuf, uint32_t dwLen);

static int __init ShTps_Init(void);
static void __exit ShTps_Exit(void);
static int ShTps_Command(I2cClient *poClient, unsigned int wCmd, void *pArg);
static int __devinit ShTps_Probe(I2cClient *poClient, const I2cDevID *poDevId);
static int __devexit ShTps_Remove(I2cClient *poClient);
static void ShTps_Connect2InputSys(WorkStruct *poWork);
static InputDev *ShTps_CreateInputDev(I2cTpsRec *poTpsRec);
static int ShTps_ConfigGPIO(I2cTpsRec *poTpsRec);
static int ShTps_ReleaseGPIO(I2cTpsRec *poTpsRec);
static void ShTps_SHLCDCPower(int nOnOff);
static void ShTps_Reset(int nOnOff);
static void ShTps_Standby(int nOnOff);
static void ShTps_HsspClk(int nOnOff);
static void ShTps_HsspData(int nOnOff);
static void ShTps_PowerOn(void);
static void ShTps_PowerOff(void);
static int ShTps_OpenCB(InputDev *poInDev);
static void ShTps_CloseCB(InputDev *pInDev);
static void ShTps_Shutdown(I2cTpsRec *poTpsRec);
static int ShTps_Start(I2cTpsRec *poTpsRec);
static void ShTps_Stop(I2cTpsRec *poTpsRec);
static irqreturn_t ShTps_IrqHandler(int nIrq, void *pvDevId);
static void ShTps_FetchInt(WorkStruct *poWork);
static void ShTps_Recover(WorkStruct *poWork);
static int ShTps_GetNextState(uint8_t bState);
static uint16_t ShTps_GetHypotLength(uint16_t wX, uint16_t wY);
static int ShTps_DelayEnable(void *poPt);
static int ShTps_SetState(I2cTpsRec *poTpsRec, uint8_t bMask, uint8_t bValue, uint8_t bCheck, uint8_t bResult);
static int ShTps_Enable_Phase1(I2cTpsRec *poTpsRec, uint8_t bResult);
static int ShTps_Enable_Phase2(I2cTpsRec *poTpsRec);
static int ShTps_Disable(I2cTpsRec *poTpsRec);
static int ShTps_GetFwVer(I2cTpsRec *poTpsRec);
static int ShTps_WriteFirmParam(I2cClient *poClient);
static int ShTps_TestMode_Start(I2cTpsRec *poTpsRec, uint8_t bMode);
static int ShTps_TestMode_Stop(I2cTpsRec *poTpsRec);
static int ShTps_ParamSetting(I2cTpsRec *poTpsRec, uint8_t *pParam);
/* 座標補正 */
static void ShTps_Qsort(Qsort_t *pTable, int nTop, int nEnd);
static void ShTps_RoundValue(short *pValue);
static int ShTps_SetAdjustParam(I2cTpsRec *poTpsRec, uint16_t *pParam);
static void ShTps_AdjustPt(short *pX, short *pY);
static void ShTps_KeyOn(InputDev *pInDev);
static void ShTps_KeyOff(InputDev *pInDev);
static void ShTps_WidOff(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY);
static void ShTps_WidSet(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY);
static void ShTps_WidWid(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY);
static void ShTps_Wid3Dn(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY);
static void ShTps_PosSet(InputDev *pInDev, uint16_t wPosX, uint16_t wPosY);
static void ShTps_PosErr(InputDev *pInDev, uint16_t wPosX, uint16_t wPosY);
static int ShTps_FwWrite(I2cTpsRec *poTpsRec, TpsFwData *pTpsFirmData);
static int ShTps_FwWriteMain(TpsFwData *pTpsFirmData);

static DEFINE_MUTEX(goTpsAccessMutex);

/*+-----------------------------------------------------------------------------+*/
/*|	マクロ定義																	|*/
/*+-----------------------------------------------------------------------------+*/
#define	MINMAX(min,max,val)	((min)>(val) ? (min) : ((max)<(val) ? (max) : (val)))
#define	SET_POINT(val,x1,y1)	val.x=(x1);val.y=(y1)
#define	SET_AREA(val,x1,y1,x2,y2,x3,y3,x4,y4)	\
								val.p.x=x1;val.p.y=y1;val.q.x=x2;val.q.y=y2;	\
								val.r.x=x3;val.r.y=y3;val.s.x=x4;val.s.y=y4;

static const TpsDispatch gTpsDispatch[TPS_STATE_MAX][TPS_STATE_MAX] =
{
	/* [EVENT] HOVER */
	{
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] HOVER */
		{	1,	ShTps_KeyOff,	NULL,			NULL,			},	/* [STATE] SDOWN */
		{	1,	ShTps_KeyOff,	ShTps_WidOff,	NULL,			},	/* [STATE] MDOWN */
		{	1,	ShTps_KeyOff,	ShTps_WidOff,	NULL,			},	/* [STATE] WIDE */
		{	1,	ShTps_KeyOff,	ShTps_WidOff,	NULL,			},	/* [STATE] 3DOWN */
	},
	/* [EVENT] SDOWN */
	{
		{	1,	ShTps_KeyOn,	NULL,			ShTps_PosSet,	},	/* [STATE] HOVER */
		{	1,	NULL,			NULL,			ShTps_PosSet,	},	/* [STATE] SDOWN */
		{	1,	NULL,			ShTps_WidOff,	ShTps_PosSet,	},	/* [STATE] MDOWN */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] WIDE */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] 3DOWN */
	},
	/* [EVENT] MDOWN */
	{
		{	1,	ShTps_KeyOn,	ShTps_WidSet,	ShTps_PosSet,	},	/* [STATE] HOVER */
		{	1,	NULL,			ShTps_WidSet,	ShTps_PosSet,	},	/* [STATE] SDOWN */
		{	1,	NULL,			ShTps_WidSet,	ShTps_PosSet,	},	/* [STATE] MDOWN */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] WIDE */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] 3DOWN */
	},
	/* [EVENT] WIDE */
	{
		{	1,	ShTps_KeyOn,	ShTps_WidWid,	ShTps_PosErr,	},	/* [STATE] HOVER */
		{	1,	NULL,			ShTps_WidWid,	ShTps_PosErr,	},	/* [STATE] SDOWN */
		{	1,	NULL,			ShTps_WidWid,	ShTps_PosErr,	},	/* [STATE] MDOWN */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] WIDE */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] 3DOWN */
	},
	/* [EVENT] 3DOWN */
	{
		{	1,	ShTps_KeyOn,	ShTps_Wid3Dn,	ShTps_PosErr,	},	/* [STATE] HOVER */
		{	1,	NULL,			ShTps_Wid3Dn,	ShTps_PosErr,	},	/* [STATE] SDOWN */
		{	1,	NULL,			ShTps_Wid3Dn,	ShTps_PosErr,	},	/* [STATE] MDOWN */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] WIDE */
		{	0,	NULL,			NULL,			NULL,			},	/* [STATE] 3DOWN */
	},
};

/*+-------------------------------------------------------------------------+*/
/*|	I2Cアクセス																|*/
/*+-------------------------------------------------------------------------+*/
/*+-------------------------------------------------------------------------+*/
/*|	I2Cリード																|*/
/*+-------------------------------------------------------------------------+*/
static int ShTps_I2cRead(I2cClient *poClient, uint8_t bRegAdr, uint8_t *pbBuf, uint32_t dwLen)
{
	int nResult;
	int nI;
	struct i2c_msg oMsgs[] =
		{
			[0] =
				{
					.addr	= poClient->addr,
					.flags	= 0,
					.buf	= (void *)&bRegAdr,
					.len	= 1
				},
			[1] =
				{
					.addr	= poClient->addr,
					.flags	= I2C_M_RD,
					.buf	= (void *)pbBuf,
					.len	= dwLen
				}
		};

	for(nI = 0; nI < TPS_I2C_RETRY; nI++)
	{
		nResult = i2c_transfer(poClient->adapter, oMsgs, 2);
		if(nResult > 0)
		{
			return 0;
		}
#ifdef TPS_PRNERR
		printk(KERN_DEBUG "[ShTps]I2cRead %d(%02X,reg:%02X,Data:%02X,Len:%d)=%d\n", nI, poClient->addr, bRegAdr, pbBuf[0], dwLen, nResult);
#endif	/* TPS_PRNERR */
	}
	return nResult;
}

/*+-------------------------------------------------------------------------+*/
/*|	I2Cライト																|*/
/*+-------------------------------------------------------------------------+*/
static int ShTps_I2cWriteOne(I2cClient *poClient, uint8_t bRegAdr, uint8_t bData)
{
	int nResult;
	int nI;
	uint8_t bBuff[2];
	struct i2c_msg oMsgs[] =
		{
			[0] =
				{
					.addr	= poClient->addr,
					.flags	= 0,
					.buf	= (void *)bBuff,
					.len	= 2
				}
		};

	bBuff[0] = bRegAdr;
	bBuff[1] = bData;
	for(nI = 0; nI < TPS_I2C_RETRY; nI++)
	{
		nResult = i2c_transfer(poClient->adapter, oMsgs, 1);
		if(nResult > 0)
		{
			return 0;
		}
#ifdef TPS_PRNERR
		printk(KERN_DEBUG "[ShTps]I2cWrite %d(%02X,reg:%02X,Data:%02X)=%d\n", nI, poClient->addr, bRegAdr, bData, nResult);
#endif	/* TPS_PRNERR */
	}
	return nResult;
}
static int ShTps_I2cWrite(I2cClient *poClient, uint8_t bRegAdr, uint8_t *pbBuf, uint32_t dwLen)
{
	int nResult;
	int nI;
	uint8_t bBuff[16+1];
	struct i2c_msg oMsgs[] =
		{
			[0] =
				{
					.addr	= poClient->addr,
					.flags	= 0,
					.buf	= (void *)bBuff,
					.len	= dwLen+1
				}
		};

	if(dwLen > 16)
		return -1;
	bBuff[0] = bRegAdr;
	memcpy(&bBuff[1], pbBuf, dwLen);
	for(nI = 0; nI < TPS_I2C_RETRY; nI++)
	{
		nResult = i2c_transfer(poClient->adapter, oMsgs, 1);
		if(nResult > 0)
		{
			return 0;
		}
#ifdef TPS_PRNERR
		printk(KERN_DEBUG "[ShTps]I2cWrite %d(%02X,reg:%02X,Data:%02X,Len:%d)=%d\n", nI, poClient->addr, bRegAdr, pbBuf[0], dwLen, nResult);
#endif	/* TPS_PRNERR */
	}
	return nResult;
}

static I2cTpsRec *gpoTpsRec = NULL;
static struct cdev goTpsCDev;
static struct class *gpoTpsClass;
static dev_t gnTpsDev;

static int TpsIf_Open(struct inode *pINode, struct file *poFile)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTpsIF]Open(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	return 0;
}

static int TpsIf_Ioctl(struct inode *pINode, struct file *poFile, unsigned int wCmd, unsigned long dwArg)
{
	int nResult = -EINVAL;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTpsIF]Ioctl(PID:%ld,CMD:%d,ARG:%lx)\n", sys_getpid(), wCmd, dwArg);
#endif	/* TPS_PRNLOG */
	mutex_lock(&goTpsAccessMutex);
	if(gpoTpsRec != NULL)
	{
		nResult = ShTps_Command(gpoTpsRec->mpoClient, wCmd, (void *)dwArg);
		switch(nResult)
		{
		case -1:
			nResult = -EIO;
			break;
		case -2:
			nResult = -EINVAL;
			break;
		case -3:
			nResult = -EFAULT;
			break;
		}
	}
	mutex_unlock(&goTpsAccessMutex);
	return nResult;
}

static const struct file_operations goTpsIf_Fops =
{
	.owner	= THIS_MODULE,
	.open	= TpsIf_Open,
	.ioctl	= TpsIf_Ioctl,
};

int __init TpsIf_Setup(void)
{
	dev_t nMajor = 0;
	dev_t nMinor = 0;
	int nResult;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTpsIF]Setup(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	nResult = alloc_chrdev_region(&gnTpsDev, 0, 1, TPSIF_DEV_NAME);
	if(!nResult)
	{
		nMajor = MAJOR(gnTpsDev);
		nMinor = MINOR(gnTpsDev);
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTpsIF]alloc_chrdev_region %d:%d\n", nMajor, nMinor);
#endif	/* TPS_PRNLOG */
	}
	else
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTpsIF]alloc_chrdev_region error\n");
#endif	/* TPS_PRNERR */
		return -1;
	}

	cdev_init(&goTpsCDev, &goTpsIf_Fops);

	goTpsCDev.owner = THIS_MODULE;
	goTpsCDev.ops = &goTpsIf_Fops;

	nResult = cdev_add(&goTpsCDev, gnTpsDev, 1);
	if(nResult)
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTpsIF]cdev_add error\n");
#endif	/* TPS_PRNERR */
		cdev_del(&goTpsCDev);
		return -1;
	}

	gpoTpsClass = class_create(THIS_MODULE, TPSIF_DEV_NAME);
	if(IS_ERR(gpoTpsClass))
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTpsIF]class_create error\n");
#endif	/* TPS_PRNERR */
		cdev_del(&goTpsCDev);
		return -1;
	}
	device_create(gpoTpsClass, NULL, gnTpsDev, &goTpsCDev, TPSIF_DEV_NAME);
	return 0;
}

void __exit TpsIf_Cleanup(void)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTpsIF]Cleanup(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	device_destroy(gpoTpsClass, gnTpsDev);
	class_destroy(gpoTpsClass);
	cdev_del(&goTpsCDev);
}

module_init(TpsIf_Setup);
module_exit(TpsIf_Cleanup);

/*+-------------------------------------------------------------------------+*/
/*|	タッチパネルドライバ													|*/
/*+-------------------------------------------------------------------------+*/
module_init(ShTps_Init);
module_exit(ShTps_Exit);
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("I2C TOUCH sensor driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:SH_touchpanel");

static const I2cDevID gI2cDevIdTableTps[] =
{
   { SH_TOUCH_I2C_DEVNAME, 0 },
   { }
};

MODULE_DEVICE_TABLE(i2c, gI2cDevIdTableTps);

/* I2Cドライバ呼び出し用構造体 */
static struct i2c_driver goI2cTpsDriver =
{
	.driver =
	{
		.owner = THIS_MODULE,
		.name  = SH_TOUCH_I2C_DEVNAME,
	},
	.probe	  = ShTps_Probe,
	.remove	  = __devexit_p(ShTps_Remove),
	.id_table = gI2cDevIdTableTps,
};

static int __init ShTps_Init(void)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Init(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	/* I2Cドライバ利用開始 */
	return i2c_add_driver(&goI2cTpsDriver);
}

static void __exit ShTps_Exit(void)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Exit(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	/* I2Cドライバ利用終了 */
	i2c_del_driver(&goI2cTpsDriver);
}

static int ShTps_Command(I2cClient *poClient, unsigned int wCmd, void *pArg)
{
	I2cTpsRec *poTpsRec = i2c_get_clientdata(poClient);
	int nResult;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Command(PID:%ld,CMD:%d,ARG:%lx)\n", sys_getpid(), wCmd, (long)pArg);
#endif	/* TPS_PRNLOG */
	/* デバイス非動作でも実行できるIOCTL */
	switch(wCmd)
	{
	case TPSDEV_FW_VERSION:
		return ShTps_GetFwVer(poTpsRec);
	case TPSDEV_FW_DOWNLOAD:
		/* ファームウェアアップデート実行 */
		return ShTps_FwWrite(poTpsRec, (TpsFwData*)gFirmData);
	case TPSDEV_FW_UPDATE:
		/* ファームウェアアップデート実行 */
		return ShTps_FwWrite(poTpsRec, (TpsFwData*)pArg);
	}
	/* デバイスがオープンされていないなら */
	if(poTpsRec->mbIsActive == 0)
		return -2;
	switch(wCmd)
	{
	case TPSDEV_ENABLE:
		/* タッチパネルを動作状態にする */
		return ShTps_SetState(poTpsRec, TPS_DISABLE_API, TPS_DISABLE_OFF, TPS_CHECK_ON, TPS_RETURN_ON);
	case TPSDEV_DISABLE:
		/* タッチパネルを待機状態にする */
		return ShTps_SetState(poTpsRec, TPS_DISABLE_API, TPS_DISABLE_ON, TPS_CHECK_ON, TPS_RETURN_ON);
	case TPSDEV_START_TESTMODE:
		/* テストモードを開始する */
		return ShTps_TestMode_Start(poTpsRec, 0x01);
	case TPSDEV_START_TESTMODE2:
		/* テストモードを開始する(モード２) */
		return ShTps_TestMode_Start(poTpsRec, 0x11);
	case TPSDEV_STOP_TESTMODE:
		/* テストモードを終了する */
		return ShTps_TestMode_Stop(poTpsRec);
	case TPSDEV_GET_SENSOR:
		nResult = -2;
		if(poTpsRec->mbIsTestMode)
		{
			nResult = 0;
			/* センサ値取得データをユーザー空間にコピーする */
			if(copy_to_user((uint8_t*)pArg, &gSense, 168))
			{
#ifdef TPS_PRNERR
				printk(KERN_DEBUG "[ShTps]copy_to_user Error\n");
#endif	/* TPS_PRNERR */
				nResult = -3;
			}
		}
		return nResult;
	case TPSDEV_SET_FIRMPARAM:
		return ShTps_ParamSetting(poTpsRec, (uint8_t*)pArg);
	case TPSDEV_CALIBRATION_PARAM:
		return ShTps_SetAdjustParam(poTpsRec, (uint16_t*)pArg);
	case TPSDEV_SLEEP_ON:
		return ShTps_SetState(poTpsRec, TPS_DISABLE_SLEEP, TPS_DISABLE_ON, TPS_CHECK_OFF, TPS_RETURN_OFF);
	case TPSDEV_SLEEP_OFF:
		return ShTps_SetState(poTpsRec, TPS_DISABLE_SLEEP, TPS_DISABLE_OFF, TPS_CHECK_OFF, TPS_RETURN_OFF);
	}
	return -2;
}

static int __devinit ShTps_Probe(I2cClient *poClient, const I2cDevID *poDevId)
{
	struct msm_sh_i2ctps_platform_data *poSetupData;
	I2cTpsRec *poTpsRec = NULL;
	int nResult;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Probe(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	if(!poClient->dev.platform_data)
	{
		dev_err(&poClient->dev, "platform device data is required\n");
		return -ENODEV;
	}
	/* タッチパネルドライバ情報用メモリを確保する */
	poTpsRec = kzalloc(sizeof(I2cTpsRec), GFP_KERNEL);
	if(!poTpsRec)
	{
		return -ENOMEM;
	}
	gpoTpsRec = poTpsRec;
	poClient->driver = &goI2cTpsDriver;
	i2c_set_clientdata(poClient, poTpsRec);
	poTpsRec->mpoClient			 = poClient;
	poSetupData					 = poClient->dev.platform_data;
	/* セットアップ情報を得る(board-xxxx.cで定義している) */
	poTpsRec->mnIrqPin			 = poSetupData->gpio_irq;
	poTpsRec->mnHsspClkPin		 = poSetupData->gpio_hssp_clk;
	poTpsRec->mnHsspDataPin		 = poSetupData->gpio_hssp_data;
	poTpsRec->mpfPinSetupFunc	 = poSetupData->gpio_setup;
	poTpsRec->mpfPinShutdownFunc = poSetupData->gpio_shutdown;
	poTpsRec->mnState			 = TPS_STATE_HOVER;
	poTpsRec->mbAccessState		 = 0;
	poTpsRec->mbIsActive		 = 0;

	/* セマフォ初期化 */
	init_MUTEX(&sem);

	/* GPIO設定を行う */
	if(0 == (nResult = ShTps_ConfigGPIO(poTpsRec)))
	{
		INIT_WORK(&poTpsRec->moIrqWork, ShTps_FetchInt);
		dev_info(&poClient->dev, "Detected %s, attempting to initialize\n", SH_TOUCH_I2C_DEVNAME);
		snprintf(poTpsRec->mcPhysInfo, QPHYSLEN, "%s/%s/event0",
				 poClient->adapter->dev.bus_id, poClient->dev.bus_id);
		INIT_DELAYED_WORK(&poTpsRec->moCmdQ, ShTps_Connect2InputSys);
		schedule_delayed_work(&poTpsRec->moCmdQ, msecs_to_jiffies(INITDELAY_TIMER));
		device_init_wakeup(&poClient->dev, 1);
		return 0;
	}
	/* GPIOの解放 */
	ShTps_ReleaseGPIO(poTpsRec);
	kfree(poTpsRec);
	return nResult;
}

static int __devexit ShTps_Remove(I2cClient *poClient)
{
	I2cTpsRec *poTpsRec = i2c_get_clientdata(poClient);

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Remove(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	gpoTpsRec = NULL;
	dev_info(&poClient->dev, "removing driver\n");
	device_init_wakeup(&poClient->dev, 0);
	if(poTpsRec->mpoInDev)
	{
		dev_dbg(&poClient->dev, "deregister from input system\n");
		input_unregister_device(poTpsRec->mpoInDev);
		poTpsRec->mpoInDev = NULL;
	}
	ShTps_Shutdown(poTpsRec);
	ShTps_ReleaseGPIO(poTpsRec);
	kfree(poTpsRec);
	return 0;
}

static void ShTps_Connect2InputSys(WorkStruct *poWork)
{
	I2cTpsRec *poTpsRec = container_of(poWork, I2cTpsRec, moCmdQ.work);
	Device *poDev = &poTpsRec->mpoClient->dev;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Connect2InputSys(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	poTpsRec->mpoInDev = ShTps_CreateInputDev(poTpsRec);
	if(poTpsRec->mpoInDev)
	{
		if(input_register_device(poTpsRec->mpoInDev) != 0)
		{
			dev_err(poDev, "Failed to register with input system\n");
			input_free_device(poTpsRec->mpoInDev);
		}
	}
}

static InputDev *ShTps_CreateInputDev(I2cTpsRec *poTpsRec)
{
	Device *poDev = &poTpsRec->mpoClient->dev;
	InputDev *pInDev = input_allocate_device();

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]CreateInputDev(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	if(pInDev)
	{
		pInDev->name = SH_TOUCH_I2C_DEVNAME;
		pInDev->phys = poTpsRec->mcPhysInfo;
		pInDev->id.bustype = BUS_I2C;
		pInDev->id.vendor  = QCVENDOR_ID;
		pInDev->id.product = QCPRODUCT_ID;
		pInDev->id.version = QCVERSION_ID;
		pInDev->open = ShTps_OpenCB;
		pInDev->close = ShTps_CloseCB;
		/* 有効なイベントを登録 */
		__set_bit(EV_KEY, pInDev->evbit);
		__set_bit(EV_ABS, pInDev->evbit);
		__set_bit(ABS_X, pInDev->absbit);
		__set_bit(ABS_Y, pInDev->absbit);
		__set_bit(ABS_TOOL_WIDTH, pInDev->absbit);
		__set_bit(BTN_TOUCH, pInDev->keybit);
		input_set_drvdata(pInDev, poTpsRec);
		/* イベントパラメータ範囲を設定 */
		input_set_abs_params(pInDev, ABS_X, 0, SH_TOUCH_MAX_X, 0, 0);
		input_set_abs_params(pInDev, ABS_Y, 0, SH_TOUCH_MAX_Y, 0, 0);
		input_set_abs_params(pInDev, ABS_TOOL_WIDTH, 0, SH_TOUCH_MAX_DISTANCE, 0, 0);
	}
	else
	{
		dev_err(poDev, "Failed to allocate input device for %s\n", SH_TOUCH_I2C_DEVNAME);
	}
	return pInDev;
}

static int ShTps_ConfigGPIO(I2cTpsRec *poTpsRec)
{
	if(poTpsRec == NULL)
		return -EINVAL;
	return poTpsRec->mpfPinSetupFunc();
}

static int ShTps_ReleaseGPIO(I2cTpsRec *poTpsRec)
{
	if(poTpsRec == NULL)
		return -EINVAL;
	/* GPIOの解放 */
	dev_info(&poTpsRec->mpoClient->dev, "releasing gpio pins %d,%d,%d\n",
			 poTpsRec->mnIrqPin, poTpsRec->mnHsspClkPin, poTpsRec->mnHsspDataPin);
	poTpsRec->mpfPinShutdownFunc();
	return 0;
}

static void ShTps_SHLCDCPower(int nOnOff)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]SHLCDCPower(PID:%ld,OnOff:%d)\n", sys_getpid(), nOnOff);
#endif	/* TPS_PRNDEB */
	/* nOnOff = 0 ならオフ */
	if(nOnOff == 0) {
		shlcdc_api_set_power_mode(SHLCDC_DEV_TYPE_TP, SHLCDC_DEV_PWR_OFF);
	} else {
		shlcdc_api_set_power_mode(SHLCDC_DEV_TYPE_TP, SHLCDC_DEV_PWR_ON);
	}
}
static void ShTps_Reset(int nOnOff)
{
	/* nOnOff = 0 ならオフ */
	if(nOnOff == 0) {
		shlcdc_api_tp_set_psoc_reset_mode(SHLCDC_TP_PSOC_RESET_LO);	/* PSOC_RESET = L */
	} else {
		shlcdc_api_tp_set_psoc_reset_mode(SHLCDC_TP_PSOC_RESET_HI);	/* PSOC_RESET = H */
	}
}

static void ShTps_Standby(int nOnOff)
{
	/* nOnOff = 0 ならオフ */
	if(nOnOff == 0) {
		shlcdc_api_tp_set_psoc_stby_mode(SHLCDC_TP_PSOC_STBY_HI);	/* PSOC_STBY = H */
	} else {
		shlcdc_api_tp_set_psoc_stby_mode(SHLCDC_TP_PSOC_STBY_LO);	/* PSOC_STBY = L */
	}
}

static void ShTps_HsspClk(int nOnOff)
{
	/* nOnOff = 0 ならオフ */
	if(nOnOff == 0) {
		gpio_direction_output(SH_TOUCH_HSSP_CLK , 0);
	} else {
		gpio_direction_output(SH_TOUCH_HSSP_CLK , 1);
	}
}

static void ShTps_HsspData(int nOnOff)
{
	/* nOnOff = 0 ならオフ */
	if(nOnOff == 0) {
		gpio_direction_output(SH_TOUCH_HSSP_DATA, 0);
	} else {
		gpio_direction_output(SH_TOUCH_HSSP_DATA, 1);
	}
}

static void ShTps_PowerOn(void)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]PowerOn(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNDEB */
	ShTps_SHLCDCPower(1);		/* SHLCDC PowerON要求 */
	ShTps_Reset(1);
	udelay(300);
	ShTps_Reset(0);
	ShTps_Standby(0);
	ShTps_SHLCDCPower(0);		/* SHLCDC PowerOFF要求 */
	mdelay(300);
}

static void ShTps_PowerOff(void)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]PowerOff(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNDEB */
	/* スタンバイにする */
	ShTps_SHLCDCPower(1);		/* SHLCDC PowerON要求 */
	ShTps_Standby(1);
	ShTps_SHLCDCPower(0);		/* SHLCDC PowerOFF要求 */
}

static int ShTps_OpenCB(InputDev *poInDev)
{
	I2cTpsRec *poTpsRec = input_get_drvdata(poInDev);

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]OpenCB(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	dev_dbg(&poTpsRec->mpoClient->dev, "ENTRY: input_dev open callback\n");
	return ShTps_Start(poTpsRec);
}

static void ShTps_CloseCB(InputDev *pInDev)
{
	I2cTpsRec *poTpsRec = input_get_drvdata(pInDev);
	Device *poDev = &poTpsRec->mpoClient->dev;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]CloseCB(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	dev_dbg(poDev, "ENTRY: close callback\n");
	ShTps_Shutdown(poTpsRec);
}

static void ShTps_Shutdown(I2cTpsRec *poTpsRec)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Shutdown(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	/* 動作中なら */
	if(poTpsRec->mbIsActive)
	{
		ShTps_Stop(poTpsRec);
		/* ワークメモリ解放 */
		flush_work(&poTpsRec->moIrqWork);
	}
}

static int ShTps_Start(I2cTpsRec *poTpsRec)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Start(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	mutex_lock(&goTpsAccessMutex);
	/* 停止中にする */
	poTpsRec->mbIsEnable = 0;
	poTpsRec->mbAccessState |= TPS_DISABLE_API;
	poTpsRec->mbIsActive = 1;
	/* 通常モードにする */
	poTpsRec->mbIsTestMode = 0;
	poTpsRec->mbAdjustEnable = 0;
	poTpsRec->mbIsFirst = 0x03;
	/* 電源オン */
	if(0 != ShTps_SetState(poTpsRec, TPS_DISABLE_API, TPS_DISABLE_OFF, TPS_CHECK_ON, TPS_RETURN_ON))
	{
		poTpsRec->mbIsActive = 0;
		mutex_unlock(&goTpsAccessMutex);
		return -EIO;
	}
	mutex_unlock(&goTpsAccessMutex);
	return 0;
}

static void ShTps_Stop(I2cTpsRec *poTpsRec)
{
	mutex_lock(&goTpsAccessMutex);
	/* 停止中にする */
	ShTps_SetState(poTpsRec, TPS_DISABLE_API, TPS_DISABLE_ON, TPS_CHECK_ON, TPS_RETURN_OFF);
	poTpsRec->mbIsActive = 0;
	mutex_unlock(&goTpsAccessMutex);
}

static irqreturn_t ShTps_IrqHandler(int nIrq, void *pvDevId)
{
	I2cTpsRec *poTpsRec = pvDevId;

//printk(KERN_DEBUG "[ShTps]IrqHandler(PID:%ld,IRQ:%02X)\n", sys_getpid(), nIrq);
	disable_irq_nosync(nIrq);
	schedule_work(&poTpsRec->moIrqWork);
	return IRQ_HANDLED;
}

static void ShTps_FetchInt(WorkStruct *poWork)
{
	I2cTpsRec *poTpsRec = container_of(poWork, I2cTpsRec, moIrqWork);
	I2cClient *poClient = poTpsRec->mpoClient;
	InputDev *pInDev = poTpsRec->mpoInDev;
	uint8_t bData[7];
	uint16_t wPosX, wPosY;
	uint16_t wDeltaX, wDeltaY;
	int nNextState;
#ifdef TPS_PRNDEB
	const char *StaName[] = { "HOVER ", "SDOWN ", "MDOWN ", "WIDE  ", "3DOWN "};
#endif	/* TPS_PRNDEB */

//printk(KERN_DEBUG "[ShTps]FetchInt(PID:%ld)\n", sys_getpid());
	mutex_lock(&goTpsAccessMutex);
	/* テストモードなら */
	if(poTpsRec->mbIsTestMode)
	{
		/* センサ値を取得する */
		ShTps_I2cRead(poClient, 0x00, gSense, 168);
		/* 次の割り込み許可 */
		enable_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin));
		mutex_unlock(&goTpsAccessMutex);
		return;
	}
	if(poTpsRec->mbIsEnable == 0)
	{
		mutex_unlock(&goTpsAccessMutex);
		return;
	}
	/* レジスタの読み込み */
	if(0 != ShTps_I2cRead(poClient, 0x00, bData, 7))
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]i2c read error\n");
#endif	/* TPS_PRNERR */
		ShTps_Stop(poTpsRec);
		INIT_DELAYED_WORK(&poTpsRec->moCmdQ, ShTps_Recover);
		schedule_delayed_work(&poTpsRec->moCmdQ, msecs_to_jiffies(RECOVER_TIMER));
		mutex_unlock(&goTpsAccessMutex);
		return;
	}
	wPosX   = (uint16_t)bData[1] + ((uint16_t)(bData[2] & 0xf0) << 4);
	wPosY   = (uint16_t)bData[3] + ((uint16_t)(bData[2] & 0x0f) << 8);
	wDeltaX = (uint16_t)bData[4] + ((uint16_t)(bData[5] & 0xf0) << 4);
	wDeltaY = (uint16_t)bData[6] + ((uint16_t)(bData[5] & 0x0f) << 8);

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Int %02X,(%4d,%4d)(%4d,%4d)\n", bData[ 0], wPosX, wPosY, wDeltaX, wDeltaY);
#endif	/* TPS_PRNLOG */
	/* ステート遷移先を得る */
	nNextState = ShTps_GetNextState(bData[ 0]);
	/* ホバー状態以外で */
	if(nNextState != TPS_STATE_HOVER)
	{
		/* 多点押しエラー */
		if(wPosX == 0x0FFF && wPosY == 0x0FFF)
		{
			nNextState = TPS_STATE_3DOWN;
		}
		/* 広範囲押しエラー */
		else if(wPosX == 0x0DDD && wPosY == 0x0DDD)
		{
			nNextState = TPS_STATE_WIDE;
		}
		else
		{
			if(wPosX > SH_TOUCH_MAX_X)
			{
				wPosX = SH_TOUCH_MAX_X;
			}
#ifdef TPS_ROTATE_180
			wPosX   = SH_TOUCH_MAX_X - wPosX;
#endif	/* TPS_ROTATE_180 */
			if(wPosY > SH_TOUCH_MAX_Y)
			{
				wPosY = SH_TOUCH_MAX_Y;
			}
#ifdef TPS_ROTATE_180
			wPosY   = SH_TOUCH_MAX_Y - wPosY;
#endif	/* TPS_ROTATE_180 */
			/* 補正が有効なら */
			if(poTpsRec->mbAdjustEnable != 0)
			{
				/* 座標の６点補正を行う */
				ShTps_AdjustPt(&wPosX, &wPosY);
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]Adjust (%4d,%4d)\n", wPosX, wPosY);
#endif	/* TPS_PRNDEB */
			}
		}
	}
	/* 遷移できるなら */
	if(gTpsDispatch[nNextState][poTpsRec->mnState].mbValid != 0)
	{
		/* リポート[BTN_TOUCH] */
		if(gTpsDispatch[nNextState][poTpsRec->mnState].mpReportKey != NULL)
		{
			gTpsDispatch[nNextState][poTpsRec->mnState].mpReportKey(pInDev);
		}
		/* リポート[ABS_TOOL_WIDTH] */
		if(gTpsDispatch[nNextState][poTpsRec->mnState].mpReportWit != NULL)
		{
			gTpsDispatch[nNextState][poTpsRec->mnState].mpReportWit(pInDev, wDeltaX, wDeltaY);
		}
		/* リポート[ABS_X][ABS_Y] */
		if(gTpsDispatch[nNextState][poTpsRec->mnState].mpReportPos != NULL)
		{
			gTpsDispatch[nNextState][poTpsRec->mnState].mpReportPos(pInDev, wPosX, wPosY);
		}
		input_sync(pInDev);
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]State[%s]->[%s] OK\n", StaName[poTpsRec->mnState], StaName[nNextState]);
#endif	/* TPS_PRNDEB */
		poTpsRec->mnState = nNextState;
	}
	else
	{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]State[%s]->[%s] NG\n", StaName[poTpsRec->mnState], StaName[nNextState]);
#endif	/* TPS_PRNDEB */
	}
	/* 次の割り込み許可 */
	enable_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin));
	mutex_unlock(&goTpsAccessMutex);
}

static void ShTps_Recover(WorkStruct *poWork)
{
	int nResult;
	I2cTpsRec *poTpsRec;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Recover(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	poTpsRec = container_of(poWork, I2cTpsRec, moCmdQ.work);

	dev_info(&poTpsRec->mpoClient->dev, "keyboard recovery requested\n");

	nResult = ShTps_Start(poTpsRec);
	if(nResult != 0)
	{
		dev_err(&poTpsRec->mpoClient->dev, "recovery failed with (nResult=%d)\n", nResult);
	}
}

static int ShTps_GetNextState(uint8_t bState)
{
	if(bState & TPS_STATE_MT)
	{
		return TPS_STATE_MDOWN;
	}
	if(bState & TPS_STATE_ST)
	{
		return TPS_STATE_SDOWN;
	}
	return TPS_STATE_HOVER;
}

/*+-----------------------------------------------------------------------------+*/
/*| 2点間の距離算出																|*/
/*+-----------------------------------------------------------------------------+*/
static uint16_t ShTps_GetHypotLength(uint16_t wX, uint16_t wY)
{
	uint16_t wResult;
	uint32_t dwX, dwY;

	if(wX == 0)
		return wY;
	if(wY == 0)
		return wX;
	dwX = (uint32_t)wX * (uint32_t)wX;
	dwY = (uint32_t)wY * (uint32_t)wY;
	wResult = (uint16_t)int_sqrt(dwX + dwY);

	/* 対角線の最大値を超えていれば */
	if(wResult > SH_TOUCH_MAX_DISTANCE)
	{
		wResult = SH_TOUCH_MAX_DISTANCE;
	}
	return wResult;
}

static int ShTps_DelayEnable(void *poPt)
{
	I2cTpsRec *poTpsRec = (I2cTpsRec *)poPt;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]ShTps_DelayEnable(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	/* パワーオン処理 */
	ShTps_PowerOn();
	gnResult = ShTps_Enable_Phase2(poTpsRec);
	up(&sem);
	return 0;
}

static int ShTps_SetState(I2cTpsRec *poTpsRec, uint8_t bMask, uint8_t bValue, uint8_t bCheck, uint8_t bResult)
{
	uint8_t bNew;
	int nResult = 0;

	/* セマフォ取得 */
	down(&sem);

	bValue &= bMask;
	bNew = (poTpsRec->mbAccessState & ~bMask) | bValue;
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]SetState[%02X]->[%02X]\n", poTpsRec->mbAccessState, bNew);
#endif	/* TPS_PRNLOG */
	/* ２重チェックありなら */
	if(bCheck)
	{
		if((poTpsRec->mbAccessState & bMask) == bValue)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]StateCheck NG\n");
#endif	/* TPS_PRNERR */
			/* セマフォ解放 */
			up(&sem);
			return -2;
		}
	}
	/* 状態を記録する */
	poTpsRec->mbAccessState = bNew;
	if(poTpsRec->mbIsActive)
	{
		/* 動作OKになったら */
		if(bNew == 0x00)
		{
			nResult = ShTps_Enable_Phase1(poTpsRec, bResult);
			return nResult;
		}
		else
		{
			nResult = ShTps_Disable(poTpsRec);
		}
	}
	/* セマフォ解放 */
	up(&sem);

	return nResult;
}

static int ShTps_Enable_Phase1(I2cTpsRec *poTpsRec, uint8_t bResult)
{
	struct task_struct *p;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Enable(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	/* 初期化する */
	gnResult = 0;

	/* 現在オフ状態で阻害要因なしなら */
	if(poTpsRec->mbIsEnable == 0 && poTpsRec->mbAccessState == 0x00)
	{
		if(poTpsRec->mbIsTestMode == 0)
		{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Enable ON\n");
#endif	/* TPS_PRNLOG */
			/* スレッドで実行する */
			p = kthread_run(ShTps_DelayEnable, poTpsRec, "shtps_delayenable");
			/* 万が一スレッド起動できなければそのまま同期実行 */
			if(IS_ERR(p))
				ShTps_DelayEnable(poTpsRec);
			/* 戻り値ありなら */
			if(bResult)
			{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Result Wait... \n");
#endif	/* TPS_PRNLOG */
				down(&sem);
				up(&sem);
				return gnResult;
			}
			return 0;
		}
		/* 動作中にする */
		poTpsRec->mbIsEnable = 1;
	}

	/* セマフォ解放 */
	up(&sem);

	return 0;
}
static int ShTps_Enable_Phase2(I2cTpsRec *poTpsRec)
{
#ifdef TPS_PRNLOG
	uint8_t bData[20];
	uint16_t wPosX, wPosY;
	uint16_t wDeltaX, wDeltaY;
#endif	/* TPS_PRNLOG */
	int nResult;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]ShTps_Enable_Phase2(%d:%d)\n", poTpsRec->mbIsActive, poTpsRec->mbAccessState);
#endif	/* TPS_PRNLOG */

	/* ステートを初期化 */
	poTpsRec->mnState = TPS_STATE_HOVER;

	/* パラメータを設定する */
	nResult = ShTps_WriteFirmParam(poTpsRec->mpoClient);
	if(nResult < 0)
	{
		ShTps_PowerOff();
		return -1;
	}

	/* 割込みハンドラの登録 */
	nResult = request_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin), &ShTps_IrqHandler,
					 IRQF_TRIGGER_LOW | IRQF_DISABLED,
				     SH_TOUCH_I2C_DEVNAME, poTpsRec);
	if(nResult < 0)
	{
#ifdef TPS_PRNERR
		printk(KERN_ERR "Could not register for  %s interrupt nResult = %d)\n", SH_TOUCH_I2C_DEVNAME, nResult);
#endif	/* TPS_PRNERR */
		ShTps_PowerOff();
		return -3;
	}

	/* 初めてのイネーブルなら */
	if(poTpsRec->mbIsFirst & 0x02)
	{
#ifdef TPS_PRNLOG
		/* レジスタの読み込み */
		if(0 == ShTps_I2cRead(poTpsRec->mpoClient, 0x00, bData, 18))
		{
			wPosX   = (uint16_t)bData[1] + ((uint16_t)(bData[2] & 0xf0) << 4);
			wPosY   = (uint16_t)bData[3] + ((uint16_t)(bData[2] & 0x0f) << 8);
			wDeltaX = (uint16_t)bData[4] + ((uint16_t)(bData[5] & 0xf0) << 4);
			wDeltaY = (uint16_t)bData[6] + ((uint16_t)(bData[5] & 0x0f) << 8);
printk(KERN_DEBUG "[ShTps]State %02X\n", bData[ 0]);
printk(KERN_DEBUG "[ShTps]Pos   %03X %03X\n", wPosX, wPosY);
printk(KERN_DEBUG "[ShTps]Delta %03X %03X\n", wDeltaX, wDeltaY);
printk(KERN_DEBUG "[ShTps]0x07- %02X %02X %02X %02X\n", bData[ 7], bData[ 8], bData[ 9], bData[10]);
printk(KERN_DEBUG "[ShTps]0x0B- %02X %02X %02X %02X\n", bData[11], bData[12], bData[13], bData[14]);
printk(KERN_DEBUG "[ShTps]0x0F- %02X %02X\n", bData[15], bData[16]);
printk(KERN_DEBUG "[ShTps]Ver   %02X\n", bData[17]);
		}
#endif	/* TPS_PRNLOG */
	}
	poTpsRec->mbIsFirst= 0x00;
	/* 動作中にする */
	poTpsRec->mbIsEnable = 1;

	return 0;
}

static int ShTps_Disable(I2cTpsRec *poTpsRec)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Disable(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	/* 現在オン中で阻害要因があるなら */
	if(poTpsRec->mbIsEnable != 0 && poTpsRec->mbAccessState != 0x00)
	{
		if(poTpsRec->mbIsTestMode == 0)
		{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]Disable OFF\n");
#endif	/* TPS_PRNLOG */
			/* ホバー状態に戻す */
			if(gTpsDispatch[TPS_STATE_HOVER][poTpsRec->mnState].mbValid != 0)
			{
				/* リポート[BTN_TOUCH] */
				if(gTpsDispatch[TPS_STATE_HOVER][poTpsRec->mnState].mpReportKey != NULL)
				{
					gTpsDispatch[TPS_STATE_HOVER][poTpsRec->mnState].mpReportKey(poTpsRec->mpoInDev);
				}
				/* リポート[ABS_TOOL_WIDTH] */
				if(gTpsDispatch[TPS_STATE_HOVER][poTpsRec->mnState].mpReportWit != NULL)
				{
					gTpsDispatch[TPS_STATE_HOVER][poTpsRec->mnState].mpReportWit(poTpsRec->mpoInDev, 0, 0);
				}
				input_sync(poTpsRec->mpoInDev);
			}
			poTpsRec->mnState = TPS_STATE_HOVER;
			/* 割込み登録解除 */
			free_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin), poTpsRec);
			/* スタンバイにする */
			ShTps_PowerOff();
		}
		/* 停止中にする */
		poTpsRec->mbIsEnable = 0;
	}
	/* 初回起動なら電源オン・オフしておく */
	else if(poTpsRec->mbIsFirst & 0x01)
	{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]First\n");
#endif	/* TPS_PRNLOG */
		ShTps_PowerOn();
		ShTps_PowerOff();
		poTpsRec->mbIsFirst= 0xfe;
	}
	return 0;
}

static int ShTps_GetFwVer(I2cTpsRec *poTpsRec)
{
	uint8_t bData;

	/* オフ状態ならパワーオンする */
	if(!poTpsRec->mbIsEnable)
	{
		ShTps_PowerOn();
	}

	/* レジスタの読み込み */
	if(0 != ShTps_I2cRead(poTpsRec->mpoClient, 0x11, &bData, 1))
	{
		return -1;
	}
	/* オフ状態に戻す */
	if(!poTpsRec->mbIsEnable)
	{
		ShTps_PowerOff();
	}
	return (int)bData;
}

static int ShTps_WriteFirmParam(I2cClient *poClient)
{
	/* パラメータを初期値を書き込む */
	if(0 != ShTps_I2cWrite(poClient, 0x09, (uint8_t*)gbSetParam, 7))
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]FirmParam Error\n");
#endif	/* TPS_PRNERR */
		return -1;
	}
	return 0;
}

static int ShTps_TestMode_Start(I2cTpsRec *poTpsRec, uint8_t bMode)
{
	int nResult;
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]TestMode_Start(PID:%ld,%02X)\n", sys_getpid(), bMode);
#endif	/* TPS_PRNLOG */
	if(poTpsRec->mbIsTestMode)
	{
		/* 既にテストモード状態ならエラーを返す */
		return -2;
	}

	/* Enable状態なら */
	if(poTpsRec->mbIsEnable)
	{
		/* 割込み登録解除 */
		free_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin), poTpsRec);
	}
	else
	{
		/* 電源を入れる */
		ShTps_PowerOn();
	}
	/* テストモード */
	if(0 != ShTps_I2cWriteOne(poTpsRec->mpoClient, 0x10, bMode))
	{
		return -1;
	}
	/* クリアする */
	memset(&gSense, 0x00, 168);
	/* テストモードにする */
	poTpsRec->mbIsTestMode = 1;
	/* 割込みハンドラの登録 */
	nResult = request_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin), &ShTps_IrqHandler,
						 IRQF_TRIGGER_LOW | IRQF_DISABLED,
					     SH_TOUCH_I2C_DEVNAME, poTpsRec);
	/* ウェイト */
	mdelay(100);

	return 0;
}

static int ShTps_TestMode_Stop(I2cTpsRec *poTpsRec)
{
	int nResult;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]TestMode_Stop(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	if(!poTpsRec->mbIsTestMode)
	{
		/* テストモード状態でないならエラーを返す */
		return -2;
	}
	/* 割込み登録解除 */
	free_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin), poTpsRec);
	/* リセットして電源をオン */
	ShTps_PowerOn();

	/* Enable状態なら */
	if(poTpsRec->mbIsEnable)
	{
		/* 割込みハンドラの登録 */
		nResult = request_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin), &ShTps_IrqHandler,
							 IRQF_TRIGGER_LOW | IRQF_DISABLED,
						     SH_TOUCH_I2C_DEVNAME, poTpsRec);
		if(nResult < 0)
		{
#ifdef TPS_PRNERR
			printk(KERN_ERR "Could not register for  %s interrupt nResult = %d)\n", SH_TOUCH_I2C_DEVNAME, nResult);
#endif	/* TPS_PRNERR */
			return -3;
		}
	}
	else
	{
		/* スタンバイにする */
		ShTps_PowerOff();
	}
	/* 通常モードにする */
	poTpsRec->mbIsTestMode = 0;

	return 0;
}

static int ShTps_ParamSetting(I2cTpsRec *poTpsRec, uint8_t *pParam)
{
	int nResult = 0;

#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]ParamSetting INIT DR(%d->%d)\n", gbSetParam[4],pParam[0]);
printk(KERN_DEBUG "[ShTps]             INIT PI(%d->%d)\n", gbSetParam[6],pParam[1]);
#endif	/* TPS_PRNLOG */

	/* INIT */
	gbSetParam[4] = pParam[0];
	gbSetParam[6] = pParam[1];

	/* パラメータを設定する */
	nResult = ShTps_WriteFirmParam(poTpsRec->mpoClient);
	return nResult;
}

/*+-----------------------------------------------------------------------------+*/
/*|	座標調整																	|*/
/*+-----------------------------------------------------------------------------+*/
static void ShTps_Qsort(Qsort_t *pTable, int nTop, int nEnd)
{
    int i, j;
    int nCenter;
	Qsort_t Swap;

    i = nTop;
    j = nEnd;

    nCenter = pTable[(nTop + nEnd) / 2].mValue;

	while(1)
	{
		while (pTable[i].mValue < nCenter)
			i++;
		while (nCenter < pTable[j].mValue)
			j--;
		if(i >= j)
			break;
		memcpy(&Swap, &pTable[i], sizeof(Qsort_t));
		memcpy(&pTable[i], &pTable[j], sizeof(Qsort_t));
		memcpy(&pTable[j], &Swap, sizeof(Qsort_t));
		i++;
		j--;
	}
	if(nTop < i - 1)
		ShTps_Qsort(pTable, nTop, i - 1);
	if(j + 1 <  nEnd)
		ShTps_Qsort(pTable, j + 1, nEnd);
}

static void ShTps_RoundValue(short *pValue)
{
	Qsort_t pTable[6];
	int nI;

	for(nI = 0; nI < 6; nI++)
	{
		pTable[nI].mNo = nI;
		pTable[nI].mValue = pValue[nI];
	}
	ShTps_Qsort(pTable, 0, 5);
	pValue[pTable[0].mNo] = pValue[pTable[1].mNo];
	pValue[pTable[5].mNo] = pValue[pTable[4].mNo];
}

static int ShTps_SetAdjustParam(I2cTpsRec *poTpsRec, uint16_t *pParam)
{
	int nI;
	TpsPoint_t sD[ADJUST_POINT];
	short nDiff[2][6];

	/* 座標調整無効 */
	if(pParam == NULL)
	{
		poTpsRec->mbAdjustEnable = 0;
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]SetAdjustParam(NULL)\n");
#endif	/* TPS_PRNLOG */
		return 0;
	}
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]SetAdjustParam((%4d,%4d)(%4d,%4d)\n", pParam[ 0], pParam[ 1], pParam[ 2], pParam[ 3]);
printk(KERN_DEBUG "                      (%4d,%4d)(%4d,%4d)\n", pParam[ 4], pParam[ 5], pParam[ 6], pParam[ 7]);
printk(KERN_DEBUG "                      (%4d,%4d)(%4d,%4d))\n", pParam[ 8], pParam[ 9], pParam[10], pParam[11]);
#endif	/* TPS_PRNLOG */

	/* パラメータチェック 有効範囲(±100) */
	for(nI = 0; nI < ADJUST_POINT; nI++)
	{
		if(pParam[nI*2+0] > gBasePt[nI].x + POS_LIMIT ||
		   pParam[nI*2+0] < gBasePt[nI].x - POS_LIMIT)
			return -2;
		if(pParam[nI*2+1] > gBasePt[nI].y + POS_LIMIT ||
		   pParam[nI*2+1] < gBasePt[nI].y - POS_LIMIT)
			return -2;
	}

	/* パラメータ保存 */
	SET_POINT(gAdjustPrm[0], pParam[ 0], pParam[ 1]);
	SET_POINT(gAdjustPrm[1], pParam[ 2], pParam[ 3]);
	SET_POINT(gAdjustPrm[2], pParam[ 4], pParam[ 5]);
	SET_POINT(gAdjustPrm[3], pParam[ 6], pParam[ 7]);
	SET_POINT(gAdjustPrm[4], pParam[ 8], pParam[ 9]);
	SET_POINT(gAdjustPrm[5], pParam[10], pParam[11]);
#if 0	/* DIFF算出方法の変更 */
	/* DIFF値算出 */
	for(nI = 0; nI < ADJUST_POINT; nI++)
	{
		sD[nI].x = (gAdjustPrm[nI].x - gBasePt[nI].x) * 3 / 4;
		sD[nI].y = (gAdjustPrm[nI].y - gBasePt[nI].y) * 3 / 4;
	}
#else
	for(nI = 0; nI < ADJUST_POINT; nI++)
	{
		nDiff[0][nI] = (gAdjustPrm[nI].x - gBasePt[nI].x);
		nDiff[1][nI] = (gAdjustPrm[nI].y - gBasePt[nI].y);
	}
	/* 最大値と最小値を切り捨てる */
	ShTps_RoundValue(nDiff[0]);			/* Xの処理 */
	ShTps_RoundValue(nDiff[1]);			/* Yの処理 */
	for(nI = 0; nI < ADJUST_POINT; nI++)
	{
		sD[nI].x = nDiff[0][nI] * 75 / 100;
		sD[nI].y = nDiff[1][nI] * 75 / 100;
	}
#endif	/* DIFF算出方法の変更 */
	/* 各エリア４隅のぶれ値を保存 */
	/*                     |-------p-------| |-------q-------| |-------r-------| |-------s-------|*/
	SET_AREA(gAreaDiff[ 0], 0      , 0      , sD[0].x, 0      , 0      , sD[0].y, sD[0].x, sD[0].y);
	SET_AREA(gAreaDiff[ 1], sD[0].x, 0      , sD[1].x, 0      , sD[0].x, sD[0].y, sD[1].x, sD[1].y);
	SET_AREA(gAreaDiff[ 2], sD[1].x, 0      , 0      , 0      , sD[1].x, sD[1].y, 0      , sD[1].y);
	SET_AREA(gAreaDiff[ 3], 0      , sD[0].y, sD[0].x, sD[0].y, 0      , sD[2].y, sD[2].x, sD[2].y);
	SET_AREA(gAreaDiff[ 4], sD[0].x, sD[0].y, sD[1].x, sD[1].y, sD[2].x, sD[2].y, sD[3].x, sD[3].y);
	SET_AREA(gAreaDiff[ 5], sD[1].x, sD[1].y, 0      , sD[1].y, sD[3].x, sD[3].y, 0      , sD[3].y);
	SET_AREA(gAreaDiff[ 6], 0      , sD[2].y, sD[2].x, sD[2].y, 0      , sD[4].y, sD[4].x, sD[4].y);
	SET_AREA(gAreaDiff[ 7], sD[2].x, sD[2].y, sD[3].x, sD[3].y, sD[4].x, sD[4].y, sD[5].x, sD[5].y);
	SET_AREA(gAreaDiff[ 8], sD[3].x, sD[3].y, 0      , sD[3].y, sD[5].x, sD[5].y, 0      , sD[5].y);
	SET_AREA(gAreaDiff[ 9], 0      , sD[4].y, sD[4].x, sD[4].y, 0      , 0      , sD[4].x, 0      );
	SET_AREA(gAreaDiff[10], sD[4].x, sD[4].y, sD[5].x, sD[5].y, sD[4].x, 0      , sD[5].x, 0      );
	SET_AREA(gAreaDiff[11], sD[5].x, sD[5].y, 0      , sD[5].y, sD[5].x, 0      , 0      , 0      );
	/* 座標補正を有効にする */
	poTpsRec->mbAdjustEnable = 1;
	return 0;
}

static void ShTps_AdjustPt(short *pX, short *pY)
{
	int nI;
	int32_t lXPQ;
	int32_t lXRS;
	int32_t lX;
	int32_t lYPR;
	int32_t lYQS;
	int32_t lY;

	/* まずエリア分けする */
	for(nI = 0; nI < AREA_COUNT; nI++)
	{
		if(gAreaRect[nI].p.x <= *pX && gAreaRect[nI].s.x > *pX &&
		   gAreaRect[nI].p.y <= *pY && gAreaRect[nI].s.y > *pY)
		{
			break;
		}
	}
	/* どのエリアにも所属していないなら補正しない */
	if(nI != AREA_COUNT)
	{
		/* 座標補正を実行する */
		lXPQ = (((gAreaDiff[nI].q.x*DOUBLE_ACCURACY) - (gAreaDiff[nI].p.x*DOUBLE_ACCURACY)) /
				(gAreaRect[nI].q.x - gAreaRect[nI].p.x)) * (*pX - gAreaRect[nI].p.x) + (gAreaDiff[nI].p.x*DOUBLE_ACCURACY);
		lXRS = (((gAreaDiff[nI].s.x*DOUBLE_ACCURACY) - (gAreaDiff[nI].r.x*DOUBLE_ACCURACY)) /
				(gAreaRect[nI].s.x - gAreaRect[nI].r.x)) * (*pX - gAreaRect[nI].r.x) + (gAreaDiff[nI].r.x*DOUBLE_ACCURACY);
		lX   = ((lXRS - lXPQ) / (gAreaRect[nI].r.y - gAreaRect[nI].p.y)) * (*pY - gAreaRect[nI].p.y) + lXPQ;
		lYPR = (((gAreaDiff[nI].r.y*DOUBLE_ACCURACY) - (gAreaDiff[nI].p.y*DOUBLE_ACCURACY)) /
				(gAreaRect[nI].r.y - gAreaRect[nI].p.y)) * (*pY - gAreaRect[nI].p.y) + (gAreaDiff[nI].p.y*DOUBLE_ACCURACY);
		lYQS = (((gAreaDiff[nI].s.y*DOUBLE_ACCURACY) - (gAreaDiff[nI].q.y*DOUBLE_ACCURACY)) /
				(gAreaRect[nI].s.y - gAreaRect[nI].q.y)) * (*pY - gAreaRect[nI].q.y) + (gAreaDiff[nI].q.y*DOUBLE_ACCURACY);
		lY   = ((lYQS - lYPR) / (gAreaRect[nI].q.x - gAreaRect[nI].p.x)) * (*pX - gAreaRect[nI].p.x) + lYPR;
		*pX = *pX - (short)(lX / DOUBLE_ACCURACY);
		*pY = *pY - (short)(lY / DOUBLE_ACCURACY);
	}
	/* 範囲外に飛び出さないように最終補正 */
	*pX = MINMAX(0, SH_TOUCH_MAX_X, *pX);
	*pY = MINMAX(0, SH_TOUCH_MAX_Y, *pY);
}

static void ShTps_KeyOn(InputDev *pInDev)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]KeyOn\n");
#endif	/* TPS_PRNDEB */
	/* タッチダウン */
	input_report_key(pInDev, BTN_TOUCH, KPD_KEYPRESS);
}
static void ShTps_KeyOff(InputDev *pInDev)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]KeyOff\n");
#endif	/* TPS_PRNDEB */
	/* タッチアップ */
	input_report_key(pInDev, BTN_TOUCH, KPD_KEYRELEASE);
}
static void ShTps_WidOff(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]WidOff(0)\n");
#endif	/* TPS_PRNDEB */
	/* ツール幅を０にリセットする */
	input_report_abs(pInDev, ABS_TOOL_WIDTH, 0);
}
static void ShTps_WidSet(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY)
{
	uint16_t wDistance;

	/** LCDの最大サイズを超えた通知はなし */
	if(wDeltaX > SH_TOUCH_MAX_X) {
		wDeltaX = SH_TOUCH_MAX_X;
	} if(wDeltaY > SH_TOUCH_MAX_Y) {
		wDeltaY = SH_TOUCH_MAX_Y;
	}
	wDistance = ShTps_GetHypotLength(wDeltaX, wDeltaY);
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]WidSet(%d)\n", wDistance);
#endif	/* TPS_PRNDEB */
	input_report_abs(pInDev, ABS_TOOL_WIDTH, wDistance);
}
static void ShTps_WidWid(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]WidWid(%d)\n", TPS_ERROR_WIDE);
#endif	/* TPS_PRNDEB */
	/* エラー通知 */
	input_report_abs(pInDev, ABS_TOOL_WIDTH, TPS_ERROR_WIDE);
}
static void ShTps_Wid3Dn(InputDev *pInDev, uint16_t wDeltaX, uint16_t wDeltaY)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]Wid3Dn(%d)\n", TPS_ERROR_3DOWN);
#endif	/* TPS_PRNDEB */
	/* エラー通知 */
	input_report_abs(pInDev, ABS_TOOL_WIDTH, TPS_ERROR_3DOWN);
}
static void ShTps_PosSet(InputDev *pInDev, uint16_t wPosX, uint16_t wPosY)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]PosSet(%d,%d)\n", wPosX, wPosY);
#endif	/* TPS_PRNDEB */
	/* 座標セット */
	input_report_abs(pInDev, ABS_X, wPosX);
	input_report_abs(pInDev, ABS_Y, wPosY);
}
static void ShTps_PosErr(InputDev *pInDev, uint16_t wPosX, uint16_t wPosY)
{
#ifdef TPS_PRNDEB
printk(KERN_DEBUG "[ShTps]PosErr(%d,%d)\n", TPS_ERROR_POS_X, TPS_ERROR_POS_Y);
#endif	/* TPS_PRNDEB */
	/* エラー通知 */
	input_report_abs(pInDev, ABS_X, TPS_ERROR_POS_X);
	input_report_abs(pInDev, ABS_Y, TPS_ERROR_POS_Y);
}

/*+-----------------------------------------------------------------------------+*/
/*| ファームウェア書き換え用													|*/
/*+-----------------------------------------------------------------------------+*/
#define TARGET_DATABUFF_LEN			128
#define TRANSITION_TIMEOUT			20000
#define	BLOCKS_PER_BANK				128
#define	SECURITY_BYTES_PER_BANK		64

/* エラーコード  */
enum
{
	SUCCESS = 0,
	INIT_ERROR,
	SiID_ERROR,
	ERASE_ERROR,
	PROGRAM_ERROR,
	VERIFY_ERROR,
	SECURITY_ERROR,
	CHECKSUM_ERROR,
};

/*+-----------------------------------------------------------------------------+*/
/*| 広域変数定義																|*/
/*+-----------------------------------------------------------------------------+*/
static uint32_t gbWait = 1;

static const uint8_t target_id_v[] = {0x07, 0x64, 0x52, 0x21};	/* デバイス【CY8CTMG201-48LTXI】*/

static const uint8_t num_bits_wait_and_poll_end = 30;
static const uint8_t wait_and_poll_end[] =
	{
		0x00, 0x00, 0x00, 0x00
	};

/*ID-SETUP-1*/
static const unsigned int num_bits_id_setup1 = 594;
static const uint8_t id_setup1_v[] =
	{
		0xCA,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x0D,0xEE,0x21,0xF7,0xF0,0x27,0xDC,0x40,
		0x9F,0x70,0x01,0xFD,0xEE,0x01,0xE7,0xC1,
		0xD7,0x9F,0x20,0x7E,0x7D,0x88,0x7D,0xEE,
		0x21,0xF7,0xF0,0x07,0xDC,0x40,0x1F,0x70,
		0x01,0xFD,0xEE,0x01,0xF7,0xA0,0x1F,0xDE,
		0xA0,0x1F,0x7B,0x00,0x7D,0xE0,0x13,0xF7,
		0xC0,0x07,0xDF,0x28,0x1F,0x7D,0x18,0x7D,
		0xFE,0x25,0xC0,
	};

/* ID-SETUP-2 */
static const unsigned int num_bits_id_setup2 = 418;
static const uint8_t id_setup2_v[] =
	{
		0xDE,0xE2,0x1F,0x7F,0x02,0x7D,0xC4,0x09,
		0xF7,0x00,0x1F,0x9F,0x07,0x5E,0x7C,0x81,
		0xF9,0xF4,0x01,0xF7,0xF0,0x07,0xDC,0x40,
		0x1F,0x70,0x01,0xFD,0xEE,0x01,0xF7,0xA0,
		0x1F,0xDE,0xA0,0x1F,0x7B,0x00,0x7D,0xE0,
		0x0D,0xF7,0xC0,0x07,0xDF,0x28,0x1F,0x7D,
		0x18,0x7D,0xFE,0x25,0xC0,
	};

/* SET-BLOCK-NUM */
static const uint8_t set_block_number[] = {0x9F, 0x40, 0xE0};
static const uint8_t set_block_number_end = 0xE0;

/* CHECKSUM-SETUP */
static const unsigned int num_bits_checksum = 418;
static const uint8_t checksum_v[] =
	{
		0xDE,0xE2,0x1F,0x7F,0x02,0x7D,0xC4,0x09,
		0xF7,0x00,0x1F,0x9F,0x07,0x5E,0x7C,0x81,
		0xF9,0xF4,0x01,0xF7,0xF0,0x07,0xDC,0x40,
		0x1F,0x70,0x01,0xFD,0xEE,0x01,0xF7,0xA0,
		0x1F,0xDE,0xA0,0x1F,0x7B,0x00,0x7D,0xE0,
		0x0F,0xF7,0xC0,0x07,0xDF,0x28,0x1F,0x7D,
		0x18,0x7D,0xFE,0x25,0xC0,
	};

/* READ-CHECKSUM */
static const uint8_t read_checksum_v[] =
	{
		0xBF, 0x20, 0xDF, 0x80, 0x80,
	};

/* PROGRAM-AND-VERIFY */
static const unsigned int num_bits_program_and_verify = 440;
static const uint8_t program_and_verify_v[] =
	{
		0xDE,0xE2,0x1F,0x7F,0x02,0x7D,0xC4,0x09,
		0xF7,0x00,0x1F,0x9F,0x07,0x5E,0x7C,0x81,
		0xF9,0xF7,0x01,0xF7,0xF0,0x07,0xDC,0x40,
		0x1F,0x70,0x01,0xFD,0xEE,0x01,0xF6,0xA0,
		0x0F,0xDE,0x80,0x7F,0x7A,0x80,0x7D,0xEC,
		0x01,0xF7,0x80,0x57,0xDF,0x00,0x1F,0x7C,
		0xA0,0x7D,0xF4,0x61,0xF7,0xF8,0x97,
	};

/* ERASE */
static const unsigned int num_bits_erase_all = 396;
static const uint8_t erase_all_v[] =
	{
		0xDE,0xE2,0x1F,0x7F,0x02,0x7D,0xC4,0x09,
		0xF7,0x00,0x1F,0x9F,0x07,0x5E,0x7C,0x85,
		0xFD,0xFC,0x01,0xF7,0x10,0x07,0xDC,0x00,
		0x7F,0x7B,0x80,0x7D,0xE0,0x0B,0xF7,0xA0,
		0x1F,0xDE,0xA0,0x1F,0x7B,0x04,0x7D,0xF0,
		0x01,0xF7,0xC9,0x87,0xDF,0x48,0x1F,0x7F,
		0x89,0x70,
	};

/* SECURE */
static const unsigned int num_bits_secure = 440;
static const uint8_t secure_v[] =
	{
		0xDE,0xE2,0x1F,0x7F,0x02,0x7D,0xC4,0x09,
		0xF7,0x00,0x1F,0x9F,0x07,0x5E,0x7C,0x81,
		0xF9,0xF7,0x01,0xF7,0xF0,0x07,0xDC,0x40,
		0x1F,0x70,0x01,0xFD,0xEE,0x01,0xF6,0xA0,
		0x0F,0xDE,0x80,0x7F,0x7A,0x80,0x7D,0xEC,
		0x01,0xF7,0x80,0x27,0xDF,0x00,0x1F,0x7C,
		0xA0,0x7D,0xF4,0x61,0xF7,0xF8,0x97,
	};

/* READ-WRITE-SETUP */
static const unsigned int num_bits_read_write = 66;
static const uint8_t read_write_v[] =
	{
		0xDE,0xF0,0x1F,0x78,0x00,0x7D,0xA0,0x03,
		0xC0,
	};
/* WRITE-BYTE */
static const uint8_t write_byte_start = 0x90;
static const uint8_t write_byte_end = 0xE0;

/* READ-ID-WORD (CY8CTMG201-48LTXI) */
static const uint8_t read_id_v[] =
	{
		0xBF, 0x00, 0xDF, 0x90, 0xFF,0x30, 0xFF,0x00, 0x80,
	};

/* READ-STATUS */
static const uint8_t read_status_v[] =
	{
		0xBF, 0x00, 0x80
	};

/* SYNC-ENABLE */
static const unsigned int num_bits_sync_enable = 110;
static const uint8_t sync_enable_v[] =
	{
		0xDE,0xE2,0x1F,0x7F,0x02,0x7D,0xC4,0x09,
		0xF7,0x00,0x1F,0xDE,0xE0,0x1C,
	};
/* SYNC-DISABLE */
static const unsigned int num_bits_sync_disable = 110;
static const uint8_t sync_disable_v[] =
	{
		0xDE,0xE2,0x1F,0x71,0x00,0x7D,0xFC,0x01,
		0xF7,0x00,0x1F,0xDE,0xE0,0x1C,
	};

static int ShTps_FwWriteStep(const uint8_t *pbFwData, int *nStep);
static int ShTps_SDATACheck(void);
static void ShTps_ErrorTrap(int nErrorCode);
static void ShTps_RunClock(int nNumCycles);
static uint8_t ShTps_ReceiveBit(void);
static uint8_t ShTps_ReceiveByte(void);
static void ShTps_SendByte(uint8_t bData, int nNumBits);
static void ShTps_SendVector(const uint8_t *bVector, int nNumBits);
static int ShTps_DetectHiLoTransition(void);
static int ShTps_XRESInitializeTargetForISSP(void);
static int ShTps_VerifySiliconID(void);
static int ShTps_ReadCheckSum(uint16_t *pwSum);
static int ShTps_EraseTarget(void);
static uint16_t ShTps_LoadTarget(const uint8_t *pbFirmData, int nBlockNo);
static int ShTps_ProgramTargetBlock(int nBlockNo);
static int ShTps_SecureTargetFlash(void);

/*+-----------------------------------------------------------------------------+*/
/*| マクロの定義																|*/
/*+-----------------------------------------------------------------------------+*/
#define	SCLKHigh()			ShTps_HsspClk(1);
#define	SCLKLow()			ShTps_HsspClk(0);
#define	SetSDATAHigh()		ShTps_HsspData(1);
#define	SetSDATALow()		ShTps_HsspData(0);
#define	AssertXRES()		ShTps_Reset(1);
#define	DeassertXRES()		ShTps_Reset(0);
#define	SetSDATAHiZ()		ShTps_HsspData(0);	\
							gpio_tlmm_config(GPIO_CFG(SH_TOUCH_HSSP_DATA, 0, GPIO_INPUT , GPIO_NO_PULL, GPIO_2MA), GPIO_ENABLE);
#define	SetSDATAStrong()	gpio_tlmm_config(GPIO_CFG(SH_TOUCH_HSSP_DATA, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA), GPIO_ENABLE);
#define	SetSCLKStrong()		gpio_tlmm_config(GPIO_CFG(SH_TOUCH_HSSP_CLK, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA), GPIO_ENABLE);

static int ShTps_FwWrite(I2cTpsRec *poTpsRec, TpsFwData *pTpsFirmData)
{
	int nResult;

	if(poTpsRec->mbIsEnable)
	{
		/* 割り込み禁止にする */
		disable_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin));
	}
	/* ファームウェアアップデート実行 */
	ShTps_SHLCDCPower(1);
	nResult = ShTps_FwWriteMain(pTpsFirmData);
	ShTps_SHLCDCPower(0);
	if(poTpsRec->mbIsEnable)
	{
		/* 割り込みを再開する */
		enable_irq(MSM_GPIO_TO_INT(poTpsRec->mnIrqPin));
	}
	else
	{
		/* 電源オフ */
		ShTps_PowerOff();
	}
	return nResult;
}

static int ShTps_FwWriteMain(TpsFwData *pTpsFirmData)
{
	int nResult = 0;
	int nDownloadStep = 0;

	/* FW書換え処理をおこなう */
	do
	{
		nResult = ShTps_FwWriteStep(pTpsFirmData->bData, &nDownloadStep);
		/* エラーならエラーコールバックを実施して終了 */
		if(nResult == -1)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]Fw_Write::Error\n");
#endif	/* TPS_PRNERR */
			return -1;
		}
		msleep(1);
	} while(nDownloadStep != -1);

#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]Fw_Write::SUCCESS\n");
#endif	/* TPS_PRNERR */
	return 0;
}

static int ShTps_FwWriteStep(const uint8_t *pbFwData, int *nStep)
{
	static uint16_t wCalcSum;			/* 算出したチェックサムデータ */
	static int nBlockNo;				/* ブロックカウンター */
	uint16_t wRecvSum;					/* 受信したチェックサム */
	int nErrorCode;

	switch(*nStep)
	{
	case 0:
		(*nStep)++;
		return 0;
	case 1:		/* 初期化（リセットモード） */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]XRESInitializeTargetForISSP\n");
#endif	/* TPS_PRNLOG */
		nErrorCode = ShTps_XRESInitializeTargetForISSP();
		if(nErrorCode != SUCCESS)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]XRESInitializeTargetForISSP::Error\n");
#endif	/* TPS_PRNERR */
			ShTps_ErrorTrap(nErrorCode);
			return -1;
		}
		(*nStep)++;
		return 7;
	case 2:		/* シリコンＩＤ検証 */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]VerifySiliconID\n");
#endif	/* TPS_PRNLOG */
		nErrorCode = ShTps_VerifySiliconID();
		if(nErrorCode != SUCCESS)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]VerifySiliconID::Error\n");
#endif	/* TPS_PRNERR */
			ShTps_ErrorTrap(nErrorCode);
			return -1;
		}
		(*nStep)++;
		return 4;
	case 3:		/* チェックサム */
		wRecvSum = 0;
		(*nStep)++;
		return 0;
	case 4:		/* プログラムの削除 */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]EraseTarget\n");
#endif	/* TPS_PRNLOG */
		nErrorCode = ShTps_EraseTarget();
		if(nErrorCode != SUCCESS)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]EraseTarget::Error\n");
#endif	/* TPS_PRNERR */
			ShTps_ErrorTrap(nErrorCode);
			return -1;
		}
		(*nStep)++;
		/* プログラムブロックの書き込み＆書き込み検証の準備 */
		wCalcSum = 0;
		nBlockNo = 0;
		return 15;
	case 5:		/* プログラムブロックの書き込み＆書き込み検証 */
		/* プログラムデータをダウンロードする */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]LoadTarget [%d]\n", nBlockNo);
#endif	/* TPS_PRNLOG */
		wCalcSum += ShTps_LoadTarget(pbFwData, nBlockNo);
		/* ダウンロードしたプログラムを書き込む */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]ProgramTargetBlock Block [%d]\n", nBlockNo);
#endif	/* TPS_PRNLOG */
		nErrorCode = ShTps_ProgramTargetBlock(nBlockNo);
		if(nErrorCode != SUCCESS)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]ProgramTargetBlock::Error\n");
#endif	/* TPS_PRNERR */
			ShTps_ErrorTrap(nErrorCode);
			return -1;
		}
		nBlockNo++;
		/* 最後まで進んだら次の処理へ */
		if(nBlockNo == BLOCKS_PER_BANK)
		{
			(*nStep)++;
			/* セキュリティ書き込みの準備 */
			nBlockNo = 0;
		}
		return 14;
	case 6:		/* セキュリティ書き込み */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]SecureTargetFlash [%d]\n", nBlockNo);
#endif	/* TPS_PRNLOG */
		nErrorCode = ShTps_SecureTargetFlash();
		if(nErrorCode != SUCCESS)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]SecureTargetFlash::Error\n");
#endif	/* TPS_PRNERR */
			ShTps_ErrorTrap(nErrorCode);
			return -1;
		}
		nBlockNo++;
		/* 最後まで進んだら次の処理へ */
		if(nBlockNo == BLOCKS_PER_BANK)
		{
			(*nStep)++;
		}
		return 25;
	case 7:		/* チェックサム検証 */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]ReadCheckSum\n");
#endif	/* TPS_PRNLOG */
		wRecvSum = 0;
		nErrorCode = ShTps_ReadCheckSum(&wRecvSum);
		if(nErrorCode != SUCCESS)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]ReadCheckSum::Error\n");
#endif	/* TPS_PRNERR */
			ShTps_ErrorTrap(nErrorCode);
			return -1;
		}
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]CheckSum[%04X:%04X]\n",wRecvSum, wCalcSum);
#endif	/* TPS_PRNLOG */
		if(wRecvSum != wCalcSum)
		{
			ShTps_ErrorTrap(CHECKSUM_ERROR);
			return -1;
		}
		(*nStep)++;
		return 25;
	case 8:
		/* SLCK/SDATAを初期状態に戻す */
		SCLKLow();
		SetSDATALow();

		AssertXRES();
		udelay(300);	/* 待ち */
		DeassertXRES();
		/* 書換え後なので少し待つ */
		mdelay(300);	/* 待ち */
		(*nStep)++;
		return 0;
	case 9:
		/* 書き込んだファームからバージョンを得る */
		*nStep = -1;
		return 0;
	default:
		*nStep = -1;
		return 0;
	}
	return 0;
}

static int ShTps_SDATACheck(void)
{
	if(gpio_get_value(SH_TOUCH_HSSP_DATA) == 1) {
		return 1;
	}
	return 0;
}

static void ShTps_ErrorTrap(int nErrorCode)
{
	SCLKLow();
	SetSDATAStrong();
	SetSDATALow();
}

static void ShTps_RunClock(int nNumCycles)
{
	int nI;

	for(nI = 0; nI < nNumCycles; nI++)
	{
		SCLKLow();
		udelay(gbWait);	/* 待ち */
		SCLKHigh();
		udelay(gbWait);	/* 待ち */
	}
}

static uint8_t ShTps_ReceiveBit(void)
{
	SCLKLow();
	udelay(gbWait);	/* 待ち */
	SCLKHigh();
	udelay(gbWait);	/* 待ち */
	return ShTps_SDATACheck();
}

static uint8_t ShTps_ReceiveByte(void)
{
	int nI;
	uint8_t bData = 0x00;

	for(nI = 0; nI < 8; nI++)
	{
		bData = (bData << 1) + ShTps_ReceiveBit();
	}
	return bData;
}

static void ShTps_SendByte(uint8_t bData, int nNumBits)
{
	int nI = 0;

	for(nI = 0; nI < nNumBits; nI++)
	{
		if(bData & 0x80)
		{
			/* Send a '1' */
			SetSDATAHigh();
			SCLKHigh();
			SCLKLow();
		}
		else
		{
			/* Send a '0' */
			SetSDATALow();
			SCLKHigh();
			SCLKLow();
		}
		bData = bData << 1;
	}
}

static void ShTps_SendVector(const uint8_t *bVector, int nNumBits)
{
	while(nNumBits > 0)
	{
		if(nNumBits >= 8)
		{
			ShTps_SendByte(*(bVector), 8);
			nNumBits -= 8;
			bVector++;
		}
		else
		{
			ShTps_SendByte(*(bVector), nNumBits);
			nNumBits = 0;
		}
	}
}

static int ShTps_DetectHiLoTransition(void)
{
	uint32_t dwTimer;

	/* Generate clocks for the target to pull SDATA High */
	dwTimer = TRANSITION_TIMEOUT * 3;
	while(1)
	{
		SCLKLow();
		udelay(gbWait);	/* 待ち */
		if (ShTps_SDATACheck())       /* exit once SDATA goes HI */
		{
			break;
		}
		SCLKHigh();
		udelay(gbWait);	/* 待ち */
		/* If the wait is too long then timeout */
		if (dwTimer-- == 0)
		{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]HiLo Timeout::1\n");
#endif	/* TPS_PRNERR */
			break;
		}
	}
	/* Generate Clocks and wait for Target to pull SDATA Low again */
	dwTimer = TRANSITION_TIMEOUT * 3;              /* reset the timeout counter */
	while(1)
	{
		SCLKLow();
		udelay(gbWait);	/* 待ち */
		if(!ShTps_SDATACheck())
		{   /* exit once SDATA returns LOW  */
			break;
		}
		SCLKHigh();
		udelay(gbWait);	/* 待ち */
		/* If the wait is too long then timeout */
		if(dwTimer-- == 0)
		{
			return -1;
		}
	}
	return 0;
}

static int ShTps_XRESInitializeTargetForISSP(void)
{
	/* Configure the pins for initialization */
	SetSDATAHiZ();
	SetSCLKStrong();
	SCLKLow();

	SetSDATAStrong();
	/* Cycle reset and put the device in programming mode when it exits reset */
	AssertXRES();
	udelay(300);	/* 待ち */
	DeassertXRES();
	/* [SETUP1] */
	ShTps_SendVector(id_setup1_v, num_bits_id_setup1);
	SetSDATAHiZ();
	if(ShTps_DetectHiLoTransition() == -1)
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]INIT_ERROR::1\n");
#endif	/* TPS_PRNERR */
		return INIT_ERROR;
	}
	SetSDATAStrong();
	ShTps_SendVector(wait_and_poll_end, num_bits_wait_and_poll_end);
	/* [SETUP2] */
	ShTps_SendVector(id_setup2_v, num_bits_id_setup2);
	SetSDATAHiZ();
	if(ShTps_DetectHiLoTransition() == -1)
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]INIT_ERROR::2\n");
#endif	/* TPS_PRNERR */
		return INIT_ERROR;
	}
	SetSDATAStrong();
	ShTps_SendVector(wait_and_poll_end, num_bits_wait_and_poll_end);

	return SUCCESS;
}

static int ShTps_VerifySiliconID(void)
{
	uint8_t bTargetID[4];

	/* [SYNC-ENABLE] */
	ShTps_SendVector(sync_enable_v, num_bits_sync_enable);
	/* [READ-ID] */
	ShTps_SendVector(read_id_v, 11);
	SetSDATAHiZ();
	ShTps_RunClock(2);
	bTargetID[0] = ShTps_ReceiveByte();	/* Silicon ID code */
	ShTps_RunClock(1);
	SetSDATAStrong();

	ShTps_SendVector(read_id_v+2, 12);
	ShTps_RunClock(2);
	SetSDATAHiZ();
	bTargetID[1] = ShTps_ReceiveByte();	/* Silicon ID code */
	ShTps_RunClock(1);
	SetSDATAStrong();

	ShTps_SendVector(read_id_v+4, 12);
	ShTps_RunClock(2);
	SetSDATAHiZ();
	bTargetID[2] = ShTps_ReceiveByte();	/* Family ID code */
	ShTps_RunClock(1);
	SetSDATAStrong();

	ShTps_SendVector(read_id_v+6, 12);
	ShTps_RunClock(2);
	SetSDATAHiZ();
	bTargetID[3] = ShTps_ReceiveByte();	/* Revision ID code */
	ShTps_RunClock(1);
	SetSDATAStrong();
	ShTps_SendVector(read_id_v+8, 1);
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]SiliconID[%02X:%02X:%02X:%02X]\n",bTargetID[0],bTargetID[1],bTargetID[2],bTargetID[3]);
#endif	/* TPS_PRNLOG */
	/* [SYNC-DISABLE] */
	ShTps_SendVector(sync_disable_v, num_bits_sync_disable);
	if((bTargetID[0] != target_id_v[0]) || (bTargetID[1] != target_id_v[1]))
	{
		return SiID_ERROR;
	}
	return SUCCESS;
}

static int ShTps_ReadCheckSum(uint16_t *pwSum)
{
	*pwSum = 0;
	/* [CHECKSUM-SETUP] */
	ShTps_SendVector(checksum_v, num_bits_checksum); 
	SetSDATAHiZ();
	if(ShTps_DetectHiLoTransition() == -1)
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]CHECKSUM_ERROR\n");
#endif	/* TPS_PRNERR */
		return CHECKSUM_ERROR;
	}
	SetSDATAStrong();
	ShTps_SendVector(wait_and_poll_end, num_bits_wait_and_poll_end);
	/* [SYNC-ENABLE] */
	ShTps_SendVector(sync_enable_v, num_bits_sync_enable);
	/* [READ-CHECKSUM] */
	ShTps_SendVector(read_checksum_v, 11);     /* first 11-bits is ReadCKSum-MSB */
	SetSDATAHiZ();
	ShTps_RunClock(2);                         /* Two SCLKs between write & read */
	*pwSum += (uint16_t)ShTps_ReceiveByte() << 8;
	ShTps_RunClock(1);                         /* See Fig. 6 */
	SetSDATAStrong();
	ShTps_SendVector(read_checksum_v + 2, 12); /* 12 bits starting from 3rd character */
	SetSDATAHiZ();
	ShTps_RunClock(2);                         /* Read-LSB Command */
	*pwSum += (uint16_t)ShTps_ReceiveByte();
	ShTps_RunClock(1);
	SetSDATAStrong();
	ShTps_SendVector(read_checksum_v + 3, 1);  /* Send the final bit of the command */
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]CheckSUM=%04X\n",*pwSum);
#endif	/* TPS_PRNLOG */
	/* [SYNC-DISABLE] */
	ShTps_SendVector(sync_disable_v, num_bits_sync_disable);
	return SUCCESS;
}

static int ShTps_EraseTarget(void)
{
	/* [ERASE] */
	ShTps_SendVector(erase_all_v, num_bits_erase_all);
	SetSDATAHiZ();
	if(ShTps_DetectHiLoTransition() == -1)
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]ERASE_ERROR\n");
#endif	/* TPS_PRNERR */
		return ERASE_ERROR;
	}
	SetSDATAStrong();
	ShTps_SendVector(wait_and_poll_end, num_bits_wait_and_poll_end);
	return SUCCESS;
}

static uint16_t ShTps_LoadTarget(const uint8_t *pbData, int nBlockNo)
{
	uint8_t bTemp;
	uint16_t wCheckSum = 0;
	uint8_t bAddr;
	int nPtr;
	int nI;

	/* [READ-WRITE] */
	ShTps_SendVector(read_write_v, num_bits_read_write);
	nPtr = nBlockNo * TARGET_DATABUFF_LEN;
	for(nI = 0, bAddr = 0x00; nI < TARGET_DATABUFF_LEN; nI++, bAddr+=2)
	{
		bTemp = pbData[nPtr+nI];
		wCheckSum += (uint16_t)bTemp;
		ShTps_SendByte(write_byte_start, 4);
		ShTps_SendByte(bAddr, 7);
		ShTps_SendByte(bTemp, 8);
		ShTps_SendByte(write_byte_end, 3);
	}
	return wCheckSum;
}

static int ShTps_ProgramTargetBlock(int nBlockNo)
{
	uint8_t bReadStatus;

	/* [SYNC-ENABLE] */
	ShTps_SendVector(sync_enable_v, num_bits_sync_enable);
	/* [SET-BLOCK_NUMBER] */
	ShTps_SendVector(set_block_number, 11);
	ShTps_SendByte((uint8_t)nBlockNo, 8);
	ShTps_SendByte(set_block_number_end, 3);
	/* [SYNC-DISABLE] */
	ShTps_SendVector(sync_disable_v, num_bits_sync_disable);
	/* [PROGRAM-AND-VERIFY] */
	ShTps_SendVector(program_and_verify_v, num_bits_program_and_verify);
	SetSDATAHiZ();
	if(ShTps_DetectHiLoTransition() == -1)
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]PROGRAM_ERROR\n");
#endif	/* TPS_PRNERR */
		return PROGRAM_ERROR;
	}
	SetSDATAStrong();
	/* [WAIT-FOR-POLL-END] */
	ShTps_SendVector(wait_and_poll_end, num_bits_wait_and_poll_end);
	/* [SYNC-ENABLE] */
	ShTps_SendVector(sync_enable_v, num_bits_sync_enable);
	/*Send Read Status vector */
	/* [READ-STATUS] */
	ShTps_SendVector(read_status_v, 11);
	SetSDATAHiZ();
	ShTps_RunClock(2);
	bReadStatus = ShTps_ReceiveByte();
	ShTps_RunClock(1);
	SetSDATAStrong();
	ShTps_SendVector(read_status_v+2, 1);
	/* [SYNC-DISABLE] */
	ShTps_SendVector(sync_disable_v, num_bits_sync_disable);
	return SUCCESS;
}

static int ShTps_SecureTargetFlash(void)
{
	uint8_t bTemp;
	uint8_t bAddr;
	int nI;

	/* [READ-WRITE] */
	ShTps_SendVector(read_write_v, num_bits_read_write);
	for(nI = 0, bAddr = 0x00; nI < SECURITY_BYTES_PER_BANK; nI++, bAddr+=2)
	{
		bTemp = 0x00;					/* Secureデータはオール0 */
		ShTps_SendByte(write_byte_start, 4);
		ShTps_SendByte(bAddr, 7);
		ShTps_SendByte(bTemp, 8);
		ShTps_SendByte(write_byte_end, 3);
	}
	/* [SECURE] */
	ShTps_SendVector(secure_v, num_bits_secure);
	SetSDATAHiZ();
	if(ShTps_DetectHiLoTransition() == -1)
	{
#ifdef TPS_PRNERR
printk(KERN_DEBUG "[ShTps]SECURITY_ERROR\n");
#endif	/* TPS_PRNERR */
		return SECURITY_ERROR;
	}
	SetSDATAStrong();
	ShTps_SendVector(wait_and_poll_end, num_bits_wait_and_poll_end);
	return SUCCESS;
}

/*+-------------------------------------------------------------------------+*/
/*|	外部公開I/F																|*/
/*+-------------------------------------------------------------------------+*/
void msm_i2ctps_flipchange(int nFlipState)
{
	mutex_lock(&goTpsAccessMutex);
	if(gpoTpsRec != NULL)
	{
		/* フリップオープンなら */
		if(nFlipState == 0x00)
			ShTps_SetState(gpoTpsRec, TPS_DISABLE_FLIP, TPS_DISABLE_OFF, TPS_CHECK_OFF, TPS_RETURN_OFF);
		else
			ShTps_SetState(gpoTpsRec, TPS_DISABLE_FLIP, TPS_DISABLE_ON, TPS_CHECK_OFF, TPS_RETURN_OFF);
	}
	mutex_unlock(&goTpsAccessMutex);
}

void msm_i2ctps_setsleep(int nIsSleep)
{
	mutex_lock(&goTpsAccessMutex);
	if(gpoTpsRec != NULL)
	{
		/* スリープ解除なら */
		if(nIsSleep == 0x00)
			ShTps_SetState(gpoTpsRec, TPS_DISABLE_SLEEP, TPS_DISABLE_OFF, TPS_CHECK_OFF, TPS_RETURN_OFF);
		else
			ShTps_SetState(gpoTpsRec, TPS_DISABLE_SLEEP, TPS_DISABLE_ON, TPS_CHECK_OFF, TPS_RETURN_OFF);
	}
	mutex_unlock(&goTpsAccessMutex);
}

void msm_i2ctps_shutdown(void)
{
#ifdef TPS_PRNLOG
printk(KERN_DEBUG "[ShTps]msm_i2ctps_shutdown(PID:%ld)\n", sys_getpid());
#endif	/* TPS_PRNLOG */
	mutex_lock(&goTpsAccessMutex);
	if(gpoTpsRec != NULL)
	{
		/* 動作中なら */
		if(gpoTpsRec->mbIsActive)
		{
			/* 停止中にする */
			ShTps_SetState(gpoTpsRec, TPS_DISABLE_API, TPS_DISABLE_ON, TPS_CHECK_ON, TPS_RETURN_OFF);
			gpoTpsRec->mbIsActive = 0;
			/* ワークメモリ解放 */
			flush_work(&gpoTpsRec->moIrqWork);
		}
		/* 電源オフシーケンス */
		ShTps_SHLCDCPower(1);		/* SHLCDC PowerON要求 */
		ShTps_Standby(0);
		ShTps_Reset(1);
		udelay(300);
		ShTps_Reset(0);
		ShTps_SHLCDCPower(0);		/* SHLCDC PowerOFF要求 */
		mdelay(300);
		ShTps_I2cWriteOne(gpoTpsRec->mpoClient, 0x10, 0x02);
	}
	mutex_unlock(&goTpsAccessMutex);
}

