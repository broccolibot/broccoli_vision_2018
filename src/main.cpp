#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <chrono>

#include <opencv2/opencv.hpp>

#include <bgrd_frame_source.hpp>
#include <folder_bgrdfs.hpp>
#include <realsense_bgrdfs.hpp>

#include <declarative_broccoli_locator.hpp>
#include <declarative_broccoli_locator_visuals.hpp>

#include <histogram.hpp>
#include <histogram_visuals.hpp>

#include <motion_server_connection.hpp>

/* TODO: 
 * Fix close-on-socket-disconnect error?!
 * Unit tests
 */

void print_usage (char* program_name) {
    fprintf(stderr, "Usage: %s <config.yml> -d <dataset_dir> -s\n"
            "\tconfig.yml: Config file directory\n"
            "\t-d : Optional dataset directory\n"
            "\t-u : Show UI components (Requires X server)\n"
            "\t-n : Disable networking (Just prints results)\n"
            , program_name);
}
int main (int argc, char** argv) {
    // Command line arguments
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    optind = 2; //Skip config dir
    char* config_dir = argv[1];
    bool enable_networking = true;
    bool show_visuals = false;
    char dataset_dir[1024] = {0};
    bool read_from_folder = false;

    int opt;
    while ((opt = getopt(argc, argv, "d:un")) != -1) {
        switch (opt) {
            case 'n': enable_networking = false; break;
            case 'u': show_visuals = true; break;
            case 'd': read_from_folder = true;
                strncpy(dataset_dir, optarg, 1024);
                break;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
                break;
        }
    }

    // Runtime image processing variables
    int width_reduction = 0;
    int min_hist = 1000;
    int max_hist = 4000;
    float percentile = 50.0 / 100.0;
    cv::Mat display_frame;
    BGRDFrame frameset = BGRDFrame(cv::Mat(), cv::Mat());

    // Runtime network variables
    std::string client_id = "id:vision\n";
    std::string ip = "invalid";
    int port = 5060;

    // Generic image processing elements
    BGRDFrameSource* source;
    ObjectLocator* locator;

    // Read config, set up locator
    { 
        cv::FileStorage fs(config_dir, CV_STORAGE_FORMAT_YAML | CV_STORAGE_READ);
        locator = new DeclarativeBroccoliLocator(fs);
        fs["width_reduction"] >> width_reduction;
        fs["min_hist"] >> min_hist;
        fs["max_hist"] >> max_hist;
        fs["percentile"] >> percentile;
        fs["ip"] >> ip;
        fs["port"] >> port;
        fs.release();
    }

    // Socket communication
    MotionServerConnection* sock = nullptr;
    if (enable_networking) sock = new MotionServerConnection(ip, port, client_id);

    // Redudant for now, but will become relevant with other crops and detection methods.
    DeclarativeBroccoliLocator* decl_broc_locator_cast = static_cast<DeclarativeBroccoliLocator*>(locator); 

    // Choose frame source
    if (read_from_folder) {
        source = new FolderBGRDFrameSource (dataset_dir);
    } else {
        source = new RealSenseBGRDFrameSource(cv::Size(640, 480), 30);
    }

    // Create UI primitives
    if (show_visuals) {
        if (decl_broc_locator_cast) DeclarativeBroccoliLocatorVisuals::init_sliders(*decl_broc_locator_cast, "Settings");
        cv::createTrackbar("Width reduction", "Settings", &width_reduction, 1080/2);
    }

    // Histogram, used for depth sampling
    auto depth_hist = Histogram<unsigned short>(min_hist, max_hist);

    // Runtime loop control vars
    bool run = true;
    bool paused = false;

    while (run) {
        // Retreive frames
        if (!paused) frameset = source->next().reduce_width(width_reduction);
        if (frameset.bgr.empty()) break;

        // Socket keepalive
        if (enable_networking) sock->keepalive();

        // Set up display frame
        if (show_visuals) frameset.bgr.copyTo(display_frame);

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

        cv::Rect roi = locator->locate(frameset.bgr);

        std::chrono::steady_clock::time_point locate = std::chrono::steady_clock::now();

        // Broccoli is detected
        if (roi.area() > 0) {
            cv::Mat sample = frameset.depth(roi);
            depth_hist.clear();
            depth_hist.insert_image(sample);

            unsigned short broccoli_depth = depth_hist.take_percentile(percentile);

            // Send detection
            if (enable_networking) {
                if (!!sock && sock->keepalive()) {
                    sock->send_u16(broccoli_depth);
                }
            } else {
                std::cout << broccoli_depth << std::endl;
            }
        }

        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

        std::cout << "Locate time: " << std::chrono::duration_cast<std::chrono::milliseconds>(locate - begin).count() << "ms ";
        std::cout << "Hist time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - locate).count() << "ms" <<std::endl;

        // Handle UI
        if (show_visuals) {
            // Declarative broccoli detector ONLY
            if (!!decl_broc_locator_cast) {
                DeclarativeBroccoliLocatorVisuals::show_internals(*decl_broc_locator_cast);
                DeclarativeBroccoliLocatorVisuals::draw_contours(*decl_broc_locator_cast, display_frame, cv::Scalar(128, 0, 255));
            }

            // Display a box around the ROI
            cv::rectangle(display_frame, roi, cv::Scalar(0, 255, 128), 1);
            imshow("display_frame", display_frame);
            //HistogramVisuals<unsigned short>::show_internals(depth_hist, "histogram");

            // Handle UI input, display callbacks
            switch (cv::waitKey(1) & 0xFF) {
                case 'q':
                    run = false;
                    break;
                case ' ':
                    paused = !paused;
                    break;
            }
        }
    }

    // Save and quit
    {
        cv::FileStorage fs(config_dir, CV_STORAGE_FORMAT_YAML | CV_STORAGE_WRITE);
        locator->save_parameters(fs);
        fs << "width_reduction" << width_reduction;
        fs << "min_hist" << min_hist;
        fs << "max_hist" << max_hist;
        fs << "percentile" << percentile;
        fs << "ip" << ip;
        fs << "port" << port;
        fs.release();
    }
    return EXIT_SUCCESS;
}
