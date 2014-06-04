#include <iostream>
#include <chrono>
#include <random>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <boost/program_options.hpp>
#include "deconvolve.hpp"
#include "regularizer.hpp"
#include "convolve.hpp"
#include "psnr.hpp"

int main(int argc, char **argv) {
    namespace po = boost::program_options;
    // Variables set by program options
    std::string basename;
    std::string extension;
    std::string infilename;
    std::string outfilename;
    std::string blurfilename;
    int kerSize = 31;
    double sigma = 0.0;
    double noiseSigma = 0.0;
    double regularizerWidth = 0.0;
    double regularizerWeight = 50.0;
    std::string method;

    po::options_description options_desc("Deconvolve arguments");
    options_desc.add_options()
        ("help", "Display this help message")
        ("image", po::value<std::string>(&basename)->required(), "Name of image (without extension)")
        ("extension,e", po::value<std::string>(&extension)->default_value("pgm"), "Extension of filename")
        ("rweight", po::value<double>(&regularizerWeight)->default_value(50.0), "Regularizer weight")
        ("rwidth", po::value<double>(&regularizerWidth)->default_value(9.0), "Regularizer width")
        ("ksigma,s", po::value<double>(&sigma)->default_value(5.0), "Kernel sigma")
        ("nsigma", po::value<double>(&noiseSigma)->default_value(5.0), "Noise sigma")
        ("method,m", po::value<std::string>(&method)->default_value("dual"), "Optimization method")
    ;

    po::positional_options_description popts_desc;
    popts_desc.add("image", 1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
            options(options_desc).positional(popts_desc).run(), vm);

    if (vm.count("help") != 0) {
        std::cout << "Usage: image-deconvolve [options] basename\n";
        std::cout << options_desc;
        exit(0);
    }
    try {
        po::notify(vm);
    } catch (std::exception& e) {
        std::cout << "Parsing error: " << e.what() << "\n";
        std::cout << "Usage: image-deconvolve [options] basename\n";
        std::cout << options_desc;
        exit(-1);
    }
    infilename = basename + "." + extension;
    outfilename = basename + "-out.pgm";
    blurfilename = basename + "-blur.pgm";

    cv::Mat image = cv::imread(infilename.c_str(), CV_LOAD_IMAGE_GRAYSCALE);
    if (!image.data) {
        std::cout << "Could not load image: " << infilename << "\n";
        exit(-1);
    }

    cv::Mat orig = image.clone();

    auto width = image.cols;
    auto height = image.rows;
    assert(image.type() == CV_8UC1);

    deconvolution::Array<2> y{boost::extents[width][height]};

    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            y[i][j] = image.at<unsigned char>(j,i);
        }
    }

    deconvolution::Array<2> ker{boost::extents[kerSize][kerSize]};
    std::vector<int> kerBase{-10, -10};
    ker.reindex(kerBase);
    for (int i = ker.index_bases()[0];
            i != ker.index_bases()[0] + int(ker.shape()[0]);
            ++i) {
        for (int j = ker.index_bases()[1];
                j != ker.index_bases()[1] + int(ker.shape()[1]);
                ++j) {
            ker[i][j] = exp(-(i*i + j*j)/(2*sigma*sigma))/(2*3.14159*sigma*sigma);
        }
    }



    std::cout << "Convolving\n";
    auto blur = convolveFFT(y, ker);

    std::mt19937 randGen;
    std::normal_distribution<double> norm{0, noiseSigma};
    for (int i = 0; i < width; ++i)
        for (int j = 0; j < height; ++j)
            blur[i][j] += norm(randGen);

    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            image.at<unsigned char>(j,i) = blur[i][j];
        }
    }

    cv::namedWindow("Display Window", CV_WINDOW_AUTOSIZE);
    cv::imshow("Display Window", image);
    cv::waitKey(1);

    cv::imwrite(blurfilename.c_str(), image); 


    deconvolution::LinearSystem<2> H = 
        [&](const deconvolution::Array<2>& x) -> deconvolution::Array<2> {
            return convolveFFT(x, ker);
        };

    constexpr int nLabels = 16;
    constexpr double labelScale = 255.0/(nLabels-1);
    auto ep = deconvolution::SmoothEdge{regularizerWeight, regularizerWidth};
    auto R = deconvolution::GridRangeRegularizer<2, deconvolution::SmoothEdge>{
        std::vector<int>{width, height}, 
        nLabels, labelScale, ep, 255.0
    };

    cv::namedWindow("Display Window", CV_WINDOW_AUTOSIZE);
    deconvolution::ProgressCallback<2> progressCallback = 
        [&](const deconvolution::Array<2>& x, double dual, double primalData, double primalReg, double smoothing) {
            for (int i = 0; i < width; ++i) {
                for (int j = 0; j < height; ++j) {
                    image.at<unsigned char>(j,i) = x[i][j];
                }
            }

            auto p = psnrMse(image, orig);

            std::cout << "\tPSNR: " << p.first << "\n";

            cv::imshow("Display Window", image);
            cv::waitKey(1);
        };

    deconvolution::DeconvolveParams params {};
    //params.maxIterations = 50;
    deconvolution::DeconvolveStats s{};

    auto startTime = std::chrono::system_clock::now();
    std::cout << "Deconvolving\n";
    deconvolution::Array<2> deblur{boost::extents[width][height]};
    if (method == std::string("primal")) {
        const int numAnnealIters = 10;
        deblur = y;
        for (int iter = 0; iter <= numAnnealIters; ++iter) {
            double alpha = static_cast<double>(iter)/static_cast<double>(numAnnealIters);
            typedef deconvolution::ConvexCombEdge<deconvolution::SmoothEdge, deconvolution::L2Edge> AnnealEdge;
            auto epSmooth = AnnealEdge{ep, deconvolution::L2Edge{1.0/regularizerWidth}, alpha};
            auto Rsmooth = deconvolution::GridRangeRegularizer<2, AnnealEdge>{
                std::vector<int>{width, height},
                    nLabels, labelScale, epSmooth, 255.0 };

            deblur = deconvolution::DeconvolvePrimal<2>(y, H, H, Rsmooth, progressCallback, params, s, deblur);

            for (int i = 0; i < width; ++i) {
                for (int j = 0; j < height; ++j) {
                    image.at<unsigned char>(j,i) = deblur[i][j];
                }
            }

            cv::namedWindow("Display Window", CV_WINDOW_AUTOSIZE);
            cv::imshow("Display Window", image);
            cv::waitKey(1);
        }
    } else if (method == std::string("dual"))
        deblur = deconvolution::Deconvolve<2>(y, H, H, R, progressCallback, params, s);
    else {
        std::cout << "Unknown optimization method!\n";
        exit(-1);
    }
    std::cout << "Done\n";

    std::cout << "Total time:       " << std::chrono::duration<double>{std::chrono::system_clock::now() - startTime}.count() << "\n";
    std::cout << "Evaluation time:  " << s.iterTime << "\n";
    std::cout << "Regularizer time: " << s.regularizerTime << "\n";
    std::cout << "Data time:        " << s.dataTime << "\n";
    std::cout << "Unary time:       " << s.unaryTime << "\n";

    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            image.at<unsigned char>(j,i) = deblur[i][j];
        }
    }

    cv::imwrite(outfilename.c_str(), image); 

    cv::namedWindow("Display Window", CV_WINDOW_AUTOSIZE);
    cv::imshow("Display Window", image);
    cv::waitKey(3000);

    auto reblur = convolveFFT(deblur, ker);

    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            image.at<unsigned char>(j,i) = reblur[i][j];
        }
    }


    return 0;
}
