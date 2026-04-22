from base_processing_algorithm import BaseProcessingAlgorithm

import cv2
import numpy as np
import torch
from dynaphos.image_processing import canny_processor
from dynaphos.simulator import GaussianSimulator

THRESHOLD_HIGH = 200

class Canny200DefaultAlgorithm(BaseProcessingAlgorithm):
    """
    A image processing algorithm that applies the canny filter and generates phosphenes.

    #Process the input data with the canny filter and generates phosphenes.
    Args: data (np.ndarray), params (dict), coordinates visual field (Map): Input image data, parameters and coordinates of the visual field.
        #Returns: torch.Tensor: the phosphene image.

    """
    def process(self, data: np.ndarray, params: dict, simulator: GaussianSimulator):
        frame = data

        frame = cv2.GaussianBlur(frame, (3, 3), 0)
        
        #processed_img = None
        processed_img = canny_processor(frame, THRESHOLD_HIGH//2, THRESHOLD_HIGH)

        stim_pattern = simulator.sample_stimulus(processed_img, rescale=True)
        
        return stim_pattern