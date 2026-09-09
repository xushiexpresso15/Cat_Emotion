/*
 * cvapp.cpp
 *
 *  Created on: 2018�~12��4��
 *      Author: 902452
 */

#include <cstdio>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "WE2_device.h"
#include "board.h"
#include "cvapp_yolov8n_ob.h"
#include "cisdp_sensor.h"

#include "WE2_core.h"

#include "ethosu_driver.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/c/common.h"
#if TFLM2209_U55TAG2205
#include "tensorflow/lite/micro/micro_error_reporter.h"
#endif
#include "img_proc_helium.h"
#include "yolo_postprocessing.h"


#include "xprintf.h"
#include "spi_master_protocol.h"
#include "cisdp_cfg.h"
#include "memory_manage.h"
#include <send_result.h>

#define CHANGE_YOLOV8_OB_OUPUT_SHAPE 1


#define INPUT_IMAGE_CHANNELS 3

#if 1
#define YOLOV8_OB_INPUT_TENSOR_WIDTH   192
#define YOLOV8_OB_INPUT_TENSOR_HEIGHT  192
#define YOLOV8_OB_INPUT_TENSOR_CHANNEL INPUT_IMAGE_CHANNELS
#else
#define YOLOV8_OB_INPUT_TENSOR_WIDTH   224
#define YOLOV8_OB_INPUT_TENSOR_HEIGHT  224
#define YOLOV8_OB_INPUT_TENSOR_CHANNEL INPUT_IMAGE_CHANNELS
#endif

#define YOLOV8N_OB_DBG_APP_LOG 0


// #define EACH_STEP_TICK
#define TOTAL_STEP_TICK
#define YOLOV8_POST_EACH_STEP_TICK 0
uint32_t systick_1, systick_2;
uint32_t loop_cnt_1, loop_cnt_2;
#define CPU_CLK	0xffffff+1
static uint32_t capture_image_tick = 0;
#ifdef TRUSTZONE_SEC
#define U55_BASE	BASE_ADDR_APB_U55_CTRL_ALIAS
#else
#ifndef TRUSTZONE
#define U55_BASE	BASE_ADDR_APB_U55_CTRL_ALIAS
#else
#define U55_BASE	BASE_ADDR_APB_U55_CTRL
#endif
#endif


using namespace std;

namespace {

constexpr int tensor_arena_size = 1053*1024;

static uint32_t tensor_arena=0;

struct ethosu_driver ethosu_drv; /* Default Ethos-U device driver */
tflite::MicroInterpreter *yolov8n_ob_int_ptr=nullptr;
TfLiteTensor *yolov8n_ob_input, *yolov8n_ob_output, *yolov8n_ob_output2;
};

#if YOLOV8N_OB_DBG_APP_LOG
std::string coco_classes[] = {"person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};
int coco_ids[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 31,
                      32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
                      57, 58, 59, 60, 61, 62, 63, 64, 65, 67, 70, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 84, 85,
                      86, 87, 88, 89, 90};

#endif

static void _arm_npu_irq_handler(void)
{
    /* Call the default interrupt handler from the NPU driver */
    ethosu_irq_handler(&ethosu_drv);
}

/**
 * @brief  Initialises the NPU IRQ
 **/
static void _arm_npu_irq_init(void)
{
    const IRQn_Type ethosu_irqnum = (IRQn_Type)U55_IRQn;

    /* Register the EthosU IRQ handler in our vector table.
     * Note, this handler comes from the EthosU driver */
    EPII_NVIC_SetVector(ethosu_irqnum, (uint32_t)_arm_npu_irq_handler);

    /* Enable the IRQ */
    NVIC_EnableIRQ(ethosu_irqnum);

}

static int _arm_npu_init(bool security_enable, bool privilege_enable)
{
    int err = 0;

    /* Initialise the IRQ */
    _arm_npu_irq_init();

    /* Initialise Ethos-U55 device */
#if TFLM2209_U55TAG2205
	const void * ethosu_base_address = (void *)(U55_BASE);
#else 
	void * const ethosu_base_address = (void *)(U55_BASE);
#endif

    if (0 != (err = ethosu_init(
                            &ethosu_drv,             /* Ethos-U driver device pointer */
                            ethosu_base_address,     /* Ethos-U NPU's base address. */
                            NULL,       /* Pointer to fast mem area - NULL for U55. */
                            0, /* Fast mem region size. */
							security_enable,                       /* Security enable. */
							privilege_enable))) {                   /* Privilege enable. */
    	xprintf("failed to initalise Ethos-U device\n");
            return err;
        }

    xprintf("Ethos-U55 device initialised\n");

    return 0;
}


int cv_yolov8n_ob_init(bool security_enable, bool privilege_enable, uint32_t model_addr) {
	int ercode = 0;

	//set memory allocation to tensor_arena
	tensor_arena = mm_reserve_align(tensor_arena_size,0x20); //1mb
	xprintf("TA[%x]\r\n",tensor_arena);


	if(_arm_npu_init(security_enable, privilege_enable)!=0)
		return -1;

	if(model_addr != 0) {
		static const tflite::Model*yolov8n_ob_model = tflite::GetModel((const void *)model_addr);

		if (yolov8n_ob_model->version() != TFLITE_SCHEMA_VERSION) {
			xprintf(
				"[ERROR] yolov8n_ob_model's schema version %d is not equal "
				"to supported version %d\n",
				yolov8n_ob_model->version(), TFLITE_SCHEMA_VERSION);
			return -1;
		}
		else {
			xprintf("yolov8n_ob model's schema version %d\n", yolov8n_ob_model->version());
		}
		#if TFLM2209_U55TAG2205
		static tflite::MicroErrorReporter yolov8n_ob_micro_error_reporter;
		#endif
		static tflite::MicroMutableOpResolver<2> yolov8n_ob_op_resolver;

		yolov8n_ob_op_resolver.AddTranspose();
		if (kTfLiteOk != yolov8n_ob_op_resolver.AddEthosU()){
			xprintf("Failed to add Arm NPU support to op resolver.");
			return false;
		}
		#if TFLM2209_U55TAG2205
			static tflite::MicroInterpreter yolov8n_ob_static_interpreter(yolov8n_ob_model, yolov8n_ob_op_resolver,
					(uint8_t*)tensor_arena, tensor_arena_size, &yolov8n_ob_micro_error_reporter);
		#else
			static tflite::MicroInterpreter yolov8n_ob_static_interpreter(yolov8n_ob_model, yolov8n_ob_op_resolver,
					(uint8_t*)tensor_arena, tensor_arena_size);  
		#endif  


		if(yolov8n_ob_static_interpreter.AllocateTensors()!= kTfLiteOk) {
			return false;
		}
		yolov8n_ob_int_ptr = &yolov8n_ob_static_interpreter;
		yolov8n_ob_input = yolov8n_ob_static_interpreter.input(0);
		yolov8n_ob_output = yolov8n_ob_static_interpreter.output(0);
		#if CHANGE_YOLOV8_OB_OUPUT_SHAPE
			yolov8n_ob_output2 = yolov8n_ob_static_interpreter.output(1);
		#endif
	}

	xprintf("initial done\n");
	return ercode;
}



typedef struct detection_cls_yolov8{
    box bbox;
    float confidence;
    float index;

} detection_cls_yolov8;

static bool yolov8_det_comparator(detection_cls_yolov8 &pa, detection_cls_yolov8 &pb)
{
    return pa.confidence > pb.confidence;
}

static void  yolov8_NMSBoxes(std::vector<box> &boxes,std::vector<float> &confidences,float modelScoreThreshold,float modelNMSThreshold,std::vector<int>& nms_result)
{
    detection_cls_yolov8 yolov8_bbox;
    std::vector<detection_cls_yolov8> yolov8_bboxes{};
    for(int i = 0; i < boxes.size(); i++)
    {
        yolov8_bbox.bbox = boxes[i];
        yolov8_bbox.confidence = confidences[i];
        yolov8_bbox.index = i;
        yolov8_bboxes.push_back(yolov8_bbox);
    }
    sort(yolov8_bboxes.begin(), yolov8_bboxes.end(), yolov8_det_comparator);
    int updated_size = yolov8_bboxes.size();
    for(int k = 0; k < updated_size; k++)
    {
        if(yolov8_bboxes[k].confidence < modelScoreThreshold)
        {
            continue;
        }
        
        nms_result.push_back(yolov8_bboxes[k].index);
        for(int j = k + 1; j < updated_size; j++)
        {
            float iou = box_iou(yolov8_bboxes[k].bbox, yolov8_bboxes[j].bbox);
            // float iou = box_diou(yolov8_bboxes[k].bbox, yolov8_bboxes[j].bbox);
            if(iou > modelNMSThreshold)
            {
                yolov8_bboxes.erase(yolov8_bboxes.begin() + j);
                updated_size = yolov8_bboxes.size();
                j = j -1;
            }
        }

    }
}



#if CHANGE_YOLOV8_OB_OUPUT_SHAPE
static void yolov8_ob_post_processing(tflite::MicroInterpreter* static_interpreter,float modelScoreThreshold, float modelNMSThreshold, struct_yolov8_ob_algoResult *alg,	std::forward_list<el_box_t> &el_algo)
{
	// JPEG stream resolution - boxes are scaled to this space
	// (img_w/h must match the actual JPEG resolution sent to the webserver)
	uint32_t img_w = app_get_raw_width();
    uint32_t img_h = app_get_raw_height();

	// The Vela-compiled model exposes 2 combined output tensors:
	//   output(0): [1, 4,   756] - bbox distances (left,top,right,bottom) for all strides
	//   output(1): [1, 756, num_classes] - class scores for all strides
	// The swap below ensures output always = bbox tensor (dims[1]==4)
	TfLiteTensor* output   = static_interpreter->output(0);
	TfLiteTensor* output_2 = static_interpreter->output(1);

	// Ensure output = boxes tensor (dimension 1 should be 4 for [1,4,756])
	if (output->dims->data[1] != 4 && output_2->dims->data[1] == 4) {
		TfLiteTensor* temp = output;
		output   = output_2;
		output_2 = temp;
	}

	int num_classes = output_2->dims->data[2]; // e.g. 4 (angry/focus/relax/scared)

	#if YOLOV8N_OB_DBG_APP_LOG
		xprintf("bbox  tensor shape: [%d,%d,%d]\r\n",
			output->dims->data[0], output->dims->data[1], output->dims->data[2]);
		xprintf("class tensor shape: [%d,%d,%d]\r\n",
			output_2->dims->data[0], output_2->dims->data[1], output_2->dims->data[2]);
	#endif

	float output_scale    = ((TfLiteAffineQuantization*)(output->quantization.params))->scale->data[0];
	int   output_zeropoint= ((TfLiteAffineQuantization*)(output->quantization.params))->zero_point->data[0];
	float output_2_scale  = ((TfLiteAffineQuantization*)(output_2->quantization.params))->scale->data[0];
	int   output_2_zeropoint= ((TfLiteAffineQuantization*)(output_2->quantization.params))->zero_point->data[0];

	std::vector<uint16_t> class_idxs;
	std::vector<float>    confidences;
	std::vector<box>      boxes;

	// output shape: [1, 4, N_anchors]  where N_anchors = dims[2] (e.g. 756)
	// Element [0, coord, anchor] is at: coord * N_anchors + anchor
	int N_anchors = output->dims->data[2]; // 756

	for (int a = 0; a < N_anchors; a++)
	{
		// --- class scores: [1, N_anchors, num_classes] → [a, c] = a*num_classes + c
		float  maxScore      = -1.0f;
		uint16_t maxClassIdx = 0;
		for (int c = 0; c < num_classes; c++) {
			int   raw   = (int)output_2->data.int8[a * num_classes + c];
			float score = ((float)raw - (float)output_2_zeropoint) * output_2_scale;
			if (score > maxScore) { maxScore = score; maxClassIdx = (uint16_t)c; }
		}

		if (maxScore < modelScoreThreshold) continue;

		// --- bbox distances: [1, 4, N_anchors] → [coord, a] = coord * N_anchors + a
		float dist[4];
		for (int k = 0; k < 4; k++) {
			int raw  = (int)output->data.int8[k * N_anchors + a];
			dist[k]  = ((float)raw - (float)output_zeropoint) * output_scale;
		}

		box bbox;
		// The exported model outputs [cx, cy, w, h] in absolute pixel coordinates (0~192)
		bbox.x = dist[0] - (0.5f * dist[2]);  // x_min
		bbox.y = dist[1] - (0.5f * dist[3]);  // y_min
		bbox.w = dist[2];                     // width
		bbox.h = dist[3];                     // height

		boxes.push_back(bbox);
		class_idxs.push_back(maxClassIdx);
		confidences.push_back(maxScore);
	}

	#if YOLOV8N_OB_DBG_APP_LOG
		xprintf("boxes.size(): %d\r\n", (int)boxes.size());
	#endif

	std::vector<int> nms_result;
	yolov8_NMSBoxes(boxes, confidences, modelScoreThreshold, modelNMSThreshold, nms_result);

	#if YOLOV8N_OB_DBG_APP_LOG
		xprintf("nms_result.size(): %d\r\n", (int)nms_result.size());
	#endif

	float scale_factor_w = (float)img_w / (float)YOLOV8_OB_INPUT_TENSOR_WIDTH;
	float scale_factor_h = (float)img_h / (float)YOLOV8_OB_INPUT_TENSOR_HEIGHT;

	for (int i = 0; i < (int)nms_result.size(); i++)
	{
		if (!(MAX_TRACKED_YOLOV8_ALGO_RES - i)) break;
		int idx = nms_result[i];

		int32_t scaled_x = (int32_t)(boxes[idx].x * scale_factor_w);
		int32_t scaled_y = (int32_t)(boxes[idx].y * scale_factor_h);
		int32_t scaled_w = (int32_t)(boxes[idx].w * scale_factor_w);
		int32_t scaled_h = (int32_t)(boxes[idx].h * scale_factor_h);

		alg->obr[i].confidence   = confidences[idx];
		alg->obr[i].class_idx    = class_idxs[idx];
		alg->obr[i].bbox.x       = (uint32_t)scaled_x;
		alg->obr[i].bbox.y       = (uint32_t)scaled_y;
		alg->obr[i].bbox.width   = (uint32_t)scaled_w;
		alg->obr[i].bbox.height  = (uint32_t)scaled_h;

		// Clamp to image bounds
		if (scaled_x < 0) { scaled_w += scaled_x; scaled_x = 0; }
		if (scaled_y < 0) { scaled_h += scaled_y; scaled_y = 0; }
		if (scaled_w < 0) scaled_w = 0;
		if (scaled_h < 0) scaled_h = 0;
		if (scaled_x + scaled_w > (int32_t)img_w) scaled_w = img_w - scaled_x;
		if (scaled_y + scaled_h > (int32_t)img_h) scaled_h = img_h - scaled_y;

		el_box_t temp_el_box;
		temp_el_box.score  = confidences[idx] * 100;
		temp_el_box.target = class_idxs[idx];
		// webserver drawBoxes() treats (x,y) as the CENTRE of the box
		temp_el_box.x = (uint16_t)(scaled_x + scaled_w / 2);
		temp_el_box.y = (uint16_t)(scaled_y + scaled_h / 2);
		temp_el_box.w = (uint16_t)scaled_w;
		temp_el_box.h = (uint16_t)scaled_h;

		el_algo.emplace_front(temp_el_box);

		#if YOLOV8N_OB_DBG_APP_LOG
			printf("detect[%d]: class=%d conf=%.2f box=[%d,%d,%d,%d]\r\n",
				i, class_idxs[idx], confidences[idx],
				(int)(scaled_x + scaled_w/2), (int)(scaled_y + scaled_h/2),
				(int)scaled_w, (int)scaled_h);
		#endif
	}

}
#else
static void yolov8_ob_post_processing(tflite::MicroInterpreter* static_interpreter,float modelScoreThreshold, float modelNMSThreshold, struct_yolov8_ob_algoResult *alg)
{
	uint32_t img_w = app_get_raw_width();
    uint32_t img_h = app_get_raw_height();
	TfLiteTensor* output = static_interpreter->output(0);
	// init postprocessing 	
	int num_classes = output->dims->data[1] - 4;

	
	// end init
	///////////////////////
	// start postprocessing
	int nboxes=0;
	int input_w = YOLOV8_OB_INPUT_TENSOR_WIDTH;
	int input_h = YOLOV8_OB_INPUT_TENSOR_HEIGHT;

	std::vector<uint16_t> class_idxs;
	std::vector<float> confidences;
	std::vector<box> boxes;


	float output_scale = ((TfLiteAffineQuantization*)(output->quantization.params))->scale->data[0];
	int output_zeropoint = ((TfLiteAffineQuantization*)(output->quantization.params))->zero_point->data[0];
	int output_size = output->bytes;

	#if YOLOV8N_OB_DBG_APP_LOG
		// xprintf("output->dims->size: %d\r\n",output->dims->size);
		// printf("output_scale: %f\r\n",output_scale);
		// xprintf("output_zeropoint: %d\r\n",output_zeropoint);
		// xprintf("output_size: %d\r\n",output_size);
		// xprintf("output->dims->data[0]: %d\r\n",output->dims->data[0]);//1
		// xprintf("output->dims->data[1]: %d\r\n",output->dims->data[1]);//84
		// xprintf("output->dims->data[2]: %d\r\n",output->dims->data[2]);//756
	#endif
	/***
	 * dequantize the output result
	 * 
	 * 
	 ******/
	for(int dims_cnt_2 = 0; dims_cnt_2 < output->dims->data[2]; dims_cnt_2++)
	{
		float outputs_bbox_data[4];
		float maxScore = (-1);// the first four indexes are bbox information
		uint16_t maxClassIndex = 0;
		for(int dims_cnt_1 = 0; dims_cnt_1 < output->dims->data[1]; dims_cnt_1++)
		{
			int value =  output->data.int8[ dims_cnt_2 + dims_cnt_1 * output->dims->data[2]];
			
			float deq_value = ((float) value-(float)output_zeropoint) * output_scale ;
			if(dims_cnt_1<4)
			{
				deq_value *= 16.0f;
				outputs_bbox_data[dims_cnt_1] = deq_value;
			}
			else
			{
				/***
				 * find maximum Score and correspond Class idx
				 * **/
				if(maxScore < deq_value)
				{
					maxScore = deq_value;
					maxClassIndex = dims_cnt_1-4;
				}
			}

		}
		if (maxScore >= modelScoreThreshold)
		{
			box bbox;
			
			bbox.x = (outputs_bbox_data[0] - (0.5 * outputs_bbox_data[2]));
			bbox.y = (outputs_bbox_data[1] - (0.5 * outputs_bbox_data[3]));
			bbox.w =(outputs_bbox_data[2]);
			bbox.h = (outputs_bbox_data[3]);
			boxes.push_back(bbox);
			class_idxs.push_back(maxClassIndex);
			confidences.push_back(maxScore);
			
		}
	}
	#if YOLOV8N_OB_DBG_APP_LOG
		xprintf("boxes.size(): %d\r\n",boxes.size());
	#endif
	/**
	 * do nms
	 * 
	 * **/

	std::vector<int> nms_result;
	yolov8_NMSBoxes(boxes, confidences, modelScoreThreshold, modelNMSThreshold, nms_result);
	for (int i = 0; i < nms_result.size(); i++)
	{
		if(!(MAX_TRACKED_YOLOV8_ALGO_RES-i))break;
		int idx = nms_result[i];

		float scale_factor_w = (float)img_w / (float)YOLOV8_OB_INPUT_TENSOR_WIDTH; 
		float scale_factor_h = (float)img_h / (float)YOLOV8_OB_INPUT_TENSOR_HEIGHT; 
		alg->obr[i].confidence = confidences[idx];
		alg->obr[i].bbox.x = (uint32_t)(boxes[idx].x * scale_factor_w);
		alg->obr[i].bbox.y = (uint32_t)(boxes[idx].y * scale_factor_h);
		alg->obr[i].bbox.width = (uint32_t)(boxes[idx].w * scale_factor_w);
		alg->obr[i].bbox.height = (uint32_t)(boxes[idx].h * scale_factor_h);
		alg->obr[i].class_idx = class_idxs[idx];
		#if YOLOV8N_OB_DBG_APP_LOG
			printf("detect object[%d]: %s confidences: %f\r\n",i, coco_classes[class_idxs[idx]].c_str(),confidences[idx]);

		#endif
	}
}

#endif

int cv_yolov8n_ob_run(struct_yolov8_ob_algoResult *algoresult_yolov8n_ob) {
	int ercode = 0;
    float w_scale;
    float h_scale;
    uint32_t img_w = app_get_raw_width();
    uint32_t img_h = app_get_raw_height();
    uint32_t ch = app_get_raw_channels();
    uint32_t raw_addr = app_get_raw_addr();
    uint32_t expand = 0;
	std::forward_list<el_box_t> el_algo;

	#if YOLOV8N_OB_DBG_APP_LOG
    xprintf("raw info: w[%d] h[%d] ch[%d] addr[%x]\n",img_w, img_h, ch, raw_addr);
	#endif

    if(yolov8n_ob_int_ptr!= nullptr) {
		#ifdef TOTAL_STEP_TICK
			SystemGetTick(&systick_1, &loop_cnt_1);
		#endif
		#ifdef EACH_STEP_TICK
			SystemGetTick(&systick_1, &loop_cnt_1);
		#endif
    	//get image from sensor and resize
		w_scale = (float)(img_w - 1) / (YOLOV8_OB_INPUT_TENSOR_WIDTH - 1);
		h_scale = (float)(img_h - 1) / (YOLOV8_OB_INPUT_TENSOR_HEIGHT - 1);

		
		hx_lib_image_resize_BGR8U3C_to_RGB24_helium((uint8_t*)raw_addr, (uint8_t*)yolov8n_ob_input->data.data,  
		                    img_w, img_h, ch, 
                        	YOLOV8_OB_INPUT_TENSOR_WIDTH, YOLOV8_OB_INPUT_TENSOR_HEIGHT, w_scale,h_scale);
		#ifdef EACH_STEP_TICK						
			SystemGetTick(&systick_2, &loop_cnt_2);
			dbg_printf(DBG_LESS_INFO,"Tick for resize image BGR8U3C_to_RGB24_helium for yolov8 OB:[%d]\r\n",(loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2));							
		#endif

		#ifdef EACH_STEP_TICK
			SystemGetTick(&systick_1, &loop_cnt_1);
		#endif

		// //uint8 to int8
		for (int i = 0; i < yolov8n_ob_input->bytes; ++i) {
			*((int8_t *)yolov8n_ob_input->data.data+i) = *((int8_t *)yolov8n_ob_input->data.data+i) - 128;
    	}

		#ifdef EACH_STEP_TICK
		SystemGetTick(&systick_2, &loop_cnt_2);
		dbg_printf(DBG_LESS_INFO,"Tick for Invoke for uint8toint8 for YOLOV8_OB:[%d]\r\n\n",(loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2));    
		#endif	

		#ifdef EACH_STEP_TICK
		SystemGetTick(&systick_1, &loop_cnt_1);
		#endif
		TfLiteStatus invoke_status = yolov8n_ob_int_ptr->Invoke();

		#ifdef EACH_STEP_TICK
		SystemGetTick(&systick_2, &loop_cnt_2);
		#endif
		if(invoke_status != kTfLiteOk)
		{
			xprintf("yolov8 object detect invoke fail\n");
			return -1;
		}
		else
		{
			#if YOLOV8N_OB_DBG_APP_LOG
			xprintf("yolov8 object detect  invoke pass\n");
			#endif
		}
		#ifdef EACH_STEP_TICK
    		dbg_printf(DBG_LESS_INFO,"Tick for Invoke for YOLOV8_OB:[%d]\r\n\n",(loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2));    
		#endif

		#ifdef EACH_STEP_TICK
			SystemGetTick(&systick_1, &loop_cnt_1);
		#endif
		yolov8_ob_post_processing(yolov8n_ob_int_ptr, 0.50, 0.45, algoresult_yolov8n_ob, el_algo);
		#ifdef EACH_STEP_TICK
			SystemGetTick(&systick_2, &loop_cnt_2);
			dbg_printf(DBG_LESS_INFO,"Tick for Invoke for YOLOV8_OB_post_processing:[%d]\r\n\n",(loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2));    
		#endif
		#if YOLOV8N_OB_DBG_APP_LOG
			xprintf("yolov8_ob_post_processing done\r\n");
		#endif
		#ifdef TOTAL_STEP_TICK						
			SystemGetTick(&systick_2, &loop_cnt_2);
			// dbg_printf(DBG_LESS_INFO,"Tick for TOTAL YOLOV8 OB:[%d]\r\n",(loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2));		
		#endif

    }
	

#ifdef UART_SEND_ALOGO_RESEULT
	algoresult_yolov8n_ob->algo_tick = (loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2) + capture_image_tick;
uint32_t judge_case_data;
uint32_t g_trans_type;
hx_drv_swreg_aon_get_appused1(&judge_case_data);
g_trans_type = (judge_case_data>>16);
if( g_trans_type == 0 || g_trans_type == 2)// transfer type is (UART) or (UART & SPI) 
{
	//invalid dcache to let uart can send the right jpeg img out
	hx_InvalidateDCache_by_Addr((volatile void *)app_get_jpeg_addr(), sizeof(uint8_t) *app_get_jpeg_sz());

	el_img_t temp_el_jpg_img = el_img_t{};
	temp_el_jpg_img.data = (uint8_t *)app_get_jpeg_addr();
	temp_el_jpg_img.size = app_get_jpeg_sz();
	temp_el_jpg_img.width = 640; // JPEG output is 640
	temp_el_jpg_img.height = 480; // JPEG output is 480
	temp_el_jpg_img.format = EL_PIXEL_FORMAT_JPEG;
	temp_el_jpg_img.rotate = EL_PIXEL_ROTATE_0;

	send_device_id();
	// event_reply(concat_strings(", ", box_results_2_json_str(el_algo), ", ", img_2_json_str(&temp_el_jpg_img)));
	event_reply(concat_strings(", ", algo_tick_2_json_str(algoresult_yolov8n_ob->algo_tick),", ", box_results_2_json_str(el_algo), ", ", img_2_json_str(&temp_el_jpg_img)));
}
	set_model_change_by_uart();
#endif	

	SystemGetTick(&systick_1, &loop_cnt_1);
	//recapture image
	sensordplib_retrigger_capture();

	
	SystemGetTick(&systick_2, &loop_cnt_2);
	capture_image_tick = (loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2);	
	return ercode;
}

int cv_yolov8n_ob_deinit()
{
	
	return 0;
}

