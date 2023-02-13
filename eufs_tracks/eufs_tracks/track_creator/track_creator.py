import numpy as np
import cv2 as cv
import sys
import matplotlib.pyplot as plt

def linear_interpolation(track, n_points):
    #attempts to place evently spaced points along the coordinates given
    #n_points is important if it is not large enough conversoin will not work fully 
    track = track.reshape(-1, 2)
    n = track.shape[0]
    total_distance = 0
    for i in range(n-1):
        dx = track[i+1, 0] - track[i, 0]
        dy = track[i+1, 1] - track[i, 1]
        total_distance += np.sqrt(dx**2 + dy**2)
    
    spacing = total_distance / (n_points - 1)
    new_track = []
    for i in range(n-1):
        dx = track[i+1, 0] - track[i, 0]
        dy = track[i+1, 1] - track[i, 1]
        distance = np.sqrt(dx**2 + dy**2)
        n_segments = int(distance / spacing)
        for j in range(n_segments):
            x = track[i, 0] + j * dx / n_segments
            y = track[i, 1] + j * dy / n_segments
            new_track.append([x, y])
    new_track.append(track[-1, :])
    return np.array(new_track)

def rotate_track(track, angle):
    angle = np.deg2rad(angle)
    rotation_matrix = np.array([[np.cos(angle), -np.sin(angle)], [np.sin(angle), np.cos(angle)]])
    rotated_track = np.dot(track, rotation_matrix)
    return rotated_track

def remove_resolution(track, resolution):
    return track[::resolution]

def scale_track(track):
    scaled_track = track*scale_factor
    return scaled_track

def track_processing():
    img_orig = cv.imread("SaudiArabian_GP.jpg")#import image 
    threshold_level,img = cv.threshold(img_orig,155,255,0) #clean track to strictly black and white 
    img = cv.cvtColor(img, cv.COLOR_BGR2GRAY) #converts image into grayscape, cv.BGR2GRAY is a preset parameter from opencv  
    contours, hierarchy = cv.findContours(img, cv.RETR_TREE, cv.CHAIN_APPROX_SIMPLE) #find contors of two sides of track cv.RETR_TREE and cv.CHAIN_APPROX_SIMPLE are preset parameters from opencv
    return contours

#Set parameters 
n_points = 15000
scale_factor = 0.05
angle_rot = 180

contours = track_processing()

#Blue is outside of track 
blue = contours[1] #this should be the outside of the track line (if not change this number)
blue = linear_interpolation(blue, n_points)
blue = scale_track(blue)
blue = remove_resolution(blue, 55)
blue = rotate_track(blue, angle_rot)

blue_x = blue[:,0]
blue_y = blue[:,1]

#Yellow is inside of track 
yellow = contours[2] #this should be the inside of the track line (if not change this number)
yellow = linear_interpolation(yellow, n_points)
yellow = scale_track(yellow)
yellow = remove_resolution(yellow, 55)
yellow = rotate_track(yellow, angle_rot)

yellow_x = yellow[:,0]
yellow_y = yellow[:,1]

#plotting
plt.scatter(yellow_x,yellow_y)
plt.scatter(blue_x,blue_y)
plt.show()


