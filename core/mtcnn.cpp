// std
#include <vector>
#include <string>

#include "core/mtcnn.h"

using namespace std;
using namespace cv;
using namespace face;

Mtcnn::Mtcnn(const string & model_dir)
{
  // load models
  Pnet = make_shared<caffe::Net<float>>(model_dir + "/MTCNN_det1.prototxt", caffe::TEST);
  Pnet->CopyTrainedLayersFrom(model_dir + "/MTCNN_det1.caffemodel");
  Rnet = make_shared<caffe::Net<float>>(model_dir + "/MTCNN_det2.prototxt", caffe::TEST);
  Rnet->CopyTrainedLayersFrom(model_dir + "/MTCNN_det2.caffemodel");
  Onet = make_shared<caffe::Net<float>>(model_dir + "/MTCNN_det3.prototxt", caffe::TEST);
  Onet->CopyTrainedLayersFrom(model_dir + "/MTCNN_det3.caffemodel");
  #ifdef MTCNN_PRECISE_LANDMARK
	  Lnet = make_shared<caffe::Net<float>>(model_dir + "/MTCNN_det4.prototxt", caffe::TEST);
    Lnet->CopyTrainedLayersFrom(model_dir + "/MTCNN_det4.caffemodel");
  #endif // MTCNN_PRECISE_LANDMARK

  // init reference facial points
  ref_96_112.reserve(5);
  ref_96_112.emplace_back(30.2946, 51.6963);
	ref_96_112.emplace_back(65.5318, 51.5014);
	ref_96_112.emplace_back(48.0252, 71.7366);
	ref_96_112.emplace_back(33.5493, 92.3655);
	ref_96_112.emplace_back(62.7299, 92.2041);

  ref_112_112.reserve(5);
  for (const auto & pt : ref_96_112)
    ref_112_112.emplace_back(pt.x + 8.f, pt.y);
}

#include <iostream>
void imsave(const Mat & sample, const vector<Proposal> & pros, const string & path)
{
  Mat img;
  sample.copyTo(img);
  for (const auto & pro : pros)
    rectangle(img, Rect(pro.bbox.x1, pro.bbox.y1, pro.bbox.x2 - pro.bbox.x1,
      pro.bbox.y2 -  pro.bbox.y1), Scalar(255, 0, 0), 2);
  imwrite(path, img);
}

vector<FaceInfo> Mtcnn::Detect(const Mat & sample, bool precise_landmark)
{
  Mat normed_sample;
  #ifdef NORM_FARST
    sample.convertTo(normed_sample, CV_32FC3, 0.0078125, -127.5 * 0.0078125);
  #else
    sample.convertTo(normed_sample, CV_32FC3);
  #endif // NORM_FARST	

  vector<BBox> bboxes = ProposalNetwork(normed_sample);
  bboxes = RefineNetwork(normed_sample, bboxes);
  vector<FaceInfo> infos = OutputNetwork(normed_sample, bboxes);

  #ifdef MTCNN_PRECISE_LANDMARK
    if (precise_landmark)
	    LandmarkNetwork(normed_sample, infos);
  #endif // MTCNN_PRECISE_LANDMARK
    
  return move(infos);
}

Mat Mtcnn::Align(const cv::Mat & sample, const FPoints & fpts, int width)
{
  CHECK(width == 96 || width == 112) << "Face align only support width to be 96 or 112";
  Mat tform, face;
  if (width == 96)
    tform = cp2tform(fpts, ref_96_112);
  else
    tform = cp2tform(fpts, ref_112_112);
  tform = tform.colRange(0, 2).t();
  warpAffine(sample, face, tform, Size(width, 112));
  return move(face);
}

void Mtcnn::SetBatchSize(shared_ptr<caffe::Net<float>> net, const int batch_size)
{
  caffe::Blob<float>* input_layer = net->input_blobs()[0];
  vector<int> input_shape = input_layer->shape();
  input_shape[0] = batch_size;
  input_layer->Reshape(input_shape);
  net->Reshape();
}

vector<vector<Mat>> Mtcnn::WarpInputLayer(shared_ptr<caffe::Net<float>> net)
{
  vector<vector<Mat>> input_channals;
  caffe::Blob<float>* input_layer = net->input_blobs()[0];
  int width = input_layer->width();
  int height = input_layer->height();
  float* input_data = input_layer->mutable_cpu_data();

  for (int i = 0; i < input_layer->num(); ++i)
  {
    vector<Mat> channals;
    for (int j = 0; j < input_layer->channels(); ++j)
    {
      channals.emplace_back(height, width, CV_32FC1, input_data);
      input_data += width * height;
    }
    input_channals.push_back(move(channals));
  }

  return move(input_channals);
}

vector<float> Mtcnn::ScalePyramid(const int min_len)
{
  vector<float> scales;
  float max_scale = 12.0f / MTCNN_MIN_SIZE;
  float min_scale = 12.0f / std::min(min_len, MTCNN_MAX_SIZE);
  for (float scale = max_scale; scale >= min_scale; scale *= MTCNN_SCALE_FACTOR)
    scales.push_back(scale);
  return move(scales);
}

vector<Proposal> Mtcnn::GetCandidates(const float scale, const caffe::Blob<float>* scores,
  const caffe::Blob<float>* regs)
{
  int stride = 2;
  int cell_size = 12;
  int output_width = regs->width();
  int output_height = regs->height();
  vector<Proposal> pros;

  for (int i = 0; i < output_height; ++i)
    for (int j = 0; j < output_width; ++j)
      if (scores->data_at(0, 1, i, j) >= MTCNN_PNET_THRESHOLD)
      {
        // bounding box
        BBox bbox(
          j * stride / scale,	// x1
          i * stride / scale,	// y1
          (j * stride + cell_size - 1) / scale + 1,	// x2
          (i * stride + cell_size - 1) / scale + 1);	// y2
        // bbox regression
        Reg reg(
          regs->data_at(0, 0, i, j),	// reg_x1
          regs->data_at(0, 1, i, j),	// reg_y1
          regs->data_at(0, 2, i, j),	// reg_x2
          regs->data_at(0, 3, i, j));	// reg_y2
        // face confidence
        float score = scores->data_at(0, 1, i, j);
        pros.emplace_back(move(bbox), score, move(reg));
      }

  return move(pros);
}

vector<Proposal> Mtcnn::NonMaximumSuppression(vector<Proposal>& pros,
  const float threshold, const NMS_TYPE type)
{
  if (pros.size() <= 1)
    return move(pros);

  sort(pros.begin(), pros.end(),
    // Lambda function: descending order by score.
    [](const Proposal& x, const Proposal& y) -> bool { return x.score > y.score; });


  vector<Proposal> nms_pros;
  while (!pros.empty()) {
    // select maximun candidates.
    Proposal max = move(pros[0]);
    pros.erase(pros.begin());
    float max_area = (max.bbox.x2 - max.bbox.x1)
      * (max.bbox.y2 - max.bbox.y1);
    // filter out overlapped candidates in the rest.
    int idx = 0;
    while (idx < pros.size()) {
      // computer intersection.
      float x1 = std::max(max.bbox.x1, pros[idx].bbox.x1);
      float y1 = std::max(max.bbox.y1, pros[idx].bbox.y1);
      float x2 = std::min(max.bbox.x2, pros[idx].bbox.x2);
      float y2 = std::min(max.bbox.y2, pros[idx].bbox.y2);
      float overlap = 0;
      if (x1 < x2 && y1 < y2)
      {
        float inter = (x2 - x1) * (y2 - y1);
        // computer denominator.
        float outer;
        float area = (pros[idx].bbox.x2 - pros[idx].bbox.x1)
          * (pros[idx].bbox.y2 - pros[idx].bbox.y1);
        if (type == IoM)	// Intersection over Minimum
          outer = std::min(max_area, area);
        else	// Intersection over Union
          outer = max_area + area - inter;
        overlap = inter / outer;
      }

      if (overlap > threshold)	// erase overlapped candidate
        pros.erase(pros.begin() + idx);
      else
        idx++;	// check next candidate
    }
    nms_pros.push_back(move(max));
  }

  return move(nms_pros);
}

void Mtcnn::BoxRegression(vector<Proposal>& pros)
{
  for (auto& pro : pros) {
    float width = pro.bbox.x2 - pro.bbox.x1;
    float height = pro.bbox.y2 - pro.bbox.y1;
    pro.bbox.x1 += pro.reg.x1 * width;	// x1
    pro.bbox.y1 += pro.reg.y1 * height;	// y1
    pro.bbox.x2 += pro.reg.x2 * height;	// x2
    pro.bbox.y2 += pro.reg.y2 * height;	// y2
  }
}

void Mtcnn::Square(vector<BBox> & bboxes) {
  for (auto& bbox : bboxes)
    Square(bbox);
}

void Mtcnn::Square(BBox & bbox) {
  float x1 = floor(bbox.x1);
  float y1 = floor(bbox.y1);
  float x2 = ceil(bbox.x2);
  float y2 = ceil(bbox.y2);
  int diff = static_cast<int>((x2 - x1) - (y2 - y1));
  if (diff > 0) {	// width > height
    y1 -= diff / 2;
    y2 += diff / 2;
    if (diff % 2 != 0) {
      if ((bbox.y1 - y1) < (y2 - bbox.y2))
        y1 -= 1;
      else
        y2 += 1;
    }
  }
  else if (diff < 0) {	// height < width
    diff = -diff;
    x1 -= diff / 2;
    x2 += diff / 2;
    if (diff % 2 != 0) {
      if ((bbox.x1 - x1) < (x2 - bbox.x2))
        x1 -= 1;
      else
        x2 += 1;
    }
  }
  bbox.x1 = x1;
  bbox.y1 = y1;
  bbox.x2 = x2;
  bbox.y2 = y2;
}

Mat Mtcnn::CropPadding(const Mat& sample, const BBox& bbox)
{
  Rect img_rect(0, 0, sample.cols, sample.rows);
  Rect crop_rect(Point2f(bbox.x1, bbox.y1), Point2f(bbox.x2, bbox.y2));
  Rect inter_on_sample = crop_rect & img_rect;
  // shifting inter from image CS (coordinate system) to crop CS.
  Rect inter_on_crop = inter_on_sample - crop_rect.tl();

  Mat crop(crop_rect.size(), CV_32FC3, Scalar(0.0));
  sample(inter_on_sample).copyTo(crop(inter_on_crop));

  return move(crop);
}

vector<BBox> Mtcnn::ProposalNetwork(const Mat & sample)
{
  int min_len = std::min(sample.cols, sample.rows);
  vector<float> scales = ScalePyramid(min_len);
  vector<Proposal> total_pros;

  caffe::Blob<float>* input_layer = Pnet->input_blobs()[0];
  for (float scale : scales)
  {
    int height = static_cast<int>(ceil(sample.rows * scale));
    int width = static_cast<int>(ceil(sample.cols * scale));
    Mat img;
    resize(sample, img, Size(width, height));

    #ifndef NORM_FARST
      img.convertTo(img, CV_32FC3, 0.0078125, -127.5 * 0.0078125);
    #endif // !NORM_FARST

    // Reshape Net.
    input_layer->Reshape(1, 3, height, width);
    Pnet->Reshape();

    vector<Mat> channels = WarpInputLayer(Pnet)[0];
    split(img, channels);

    const vector<caffe::Blob<float>*> out = Pnet->Forward();
    vector<Proposal> pros = GetCandidates(scale, out[1], out[0]);
    
    // intra scale nms
    pros = NonMaximumSuppression(pros, 0.5f, IoU);
    if (!pros.empty()) {
      total_pros.insert(total_pros.end(), pros.begin(), pros.end());
    }
  }
  // inter scale nms
  total_pros = NonMaximumSuppression(total_pros, 0.7f, IoU);
  BoxRegression(total_pros);

  vector<BBox> bboxes;
  for (auto& pro : total_pros)
    bboxes.push_back(move(pro.bbox));

  return move(bboxes);
}

vector<BBox> Mtcnn::RefineNetwork(const Mat & sample, vector<BBox> & bboxes)
{
  if (bboxes.empty())
    return move(bboxes);

  size_t num = bboxes.size();
  Square(bboxes);	// convert bbox to square
  SetBatchSize(Rnet, num);
  vector<vector<Mat>> input_channels = WarpInputLayer(Rnet);

  for (int i = 0; i < num; ++i)
  {
    Mat crop = CropPadding(sample, bboxes[i]);
    resize(crop, crop, Size(24, 24));
    #ifndef NORM_FARST
      crop.convertTo(crop, CV_32FC3, 0.0078125, -127.5 * 0.0078125);
    #endif // NORM_FARST
    split(crop, input_channels[i]);
  }

  const vector<caffe::Blob<float>*> out = Rnet->Forward();
  caffe::Blob<float>*   regs = out[0];
  caffe::Blob<float>* scores = out[1];

  vector<Proposal> pros;
  for (int i = 0; i < num; ++i)
  {
    if (scores->data_at(i, 1, 0, 0) >= MTCNN_RNET_THRESHOLD) {
      Reg reg(
        regs->data_at(i, 0, 0, 0),	// x1
        regs->data_at(i, 1, 0, 0),	// y1
        regs->data_at(i, 2, 0, 0),	// x2
        regs->data_at(i, 3, 0, 0));	// y2
      float score = scores->data_at(i, 1, 0, 0);
      pros.emplace_back(move(bboxes[i]), score, move(reg));
    }
  }

  pros = NonMaximumSuppression(pros, 0.7f, IoU);
  BoxRegression(pros);

  bboxes.clear();
  for (auto& pro : pros)
    bboxes.push_back(move(pro.bbox));

  return move(bboxes);
}

vector<FaceInfo> Mtcnn::OutputNetwork(const Mat & sample, vector<BBox> & bboxes)
{
  vector<FaceInfo> infos;
  if (bboxes.empty())
    return move(infos);

  size_t num = bboxes.size();
  Square(bboxes);	// convert bbox to square

  SetBatchSize(Onet, num);
  vector<vector<Mat> > input_channels = WarpInputLayer(Onet);
  for (int i = 0; i < num; ++i)
  {
    Mat crop = CropPadding(sample, bboxes[i]);
    resize(crop, crop, Size(48, 48));
    #ifndef NORM_FARST
      crop.convertTo(crop, CV_32FC3, 0.0078125, -127.5 * 0.0078125);
    #endif // NORM_FARST
    split(crop, input_channels[i]);
  }

  const vector<caffe::Blob<float>*> out = Onet->Forward();
  caffe::Blob<float>* regs   = out[0];
  caffe::Blob<float>* fpts   = out[1];
  caffe::Blob<float>* scores = out[2];

  vector<Proposal> pros;
  for (int i = 0; i < num; ++i)
  {
    BBox& bbox = bboxes[i];
    if (scores->data_at(i, 1, 0, 0) >= MTCNN_ONET_THRESHOLD) {
      Reg reg(
        regs->data_at(i, 0, 0, 0),	// x1
        regs->data_at(i, 1, 0, 0),	// y1
        regs->data_at(i, 2, 0, 0),	// x2
        regs->data_at(i, 3, 0, 0));	// y2
      float score = scores->data_at(i, 1, 0, 0);
      // facial landmarks
      FPoints fpt;
      float width = bbox.x2 - bbox.x1;
      float height = bbox.y2 - bbox.y1;
      for (int j = 0; j < 5; j++)
        fpt.emplace_back(
        fpts->data_at(i, j, 0, 0) * width + bbox.x1,
        fpts->data_at(i, j+5, 0, 0) * height + bbox.y1);
      pros.emplace_back(move(bbox), score, move(fpt), move(reg));
    }
  }

  pros = NonMaximumSuppression(pros, 0.7f, IoM);
  BoxRegression(pros);

  for (auto & pro : pros)
    infos.emplace_back(move(pro.bbox), pro.score, move(pro.fpts));

  return move(infos);
}

#ifdef MTCNN_PRECISE_LANDMARK
void Mtcnn::LandmarkNetwork(const Mat & sample, vector<FaceInfo>& infos)
{
  if (infos.empty())
    return;

  size_t num = infos.size();
  SetBatchSize(Lnet, num);
  Rect img_rect(0, 0, sample.cols, sample.rows);
  vector<vector<Mat>> input_channels = WarpInputLayer(Lnet);
  Mat patch_sizes(num, 5, CV_32S);
  for (int i = 0; i < num; ++i)
  {
    FaceInfo & info = infos[i];
    float patchw = std::max(info.bbox.x2 - info.bbox.x1,
      info.bbox.y2 - info.bbox.y1);
    float patchs = patchw * 0.25f;
    for (int j = 0; j < 5; ++j)
    {
      BBox patch;
      patch.x1 = info.fpts[j].x - patchs * 0.5f;
      patch.y1 = info.fpts[j].y - patchs * 0.5f;
      patch.x2 = info.fpts[j].x + patchs * 0.5f;
      patch.y2 = info.fpts[j].y + patchs * 0.5f;
      Square(patch);
      patch_sizes.at<int>(i, j) = patch.x2 - patch.x1;
      
      Mat crop = CropPadding(sample, patch);
      resize(crop, crop, Size(24, 24));
      #ifndef NORM_FARST
        crop.convertTo(crop, CV_32FC3, 0.0078125, -127.5 * 0.0078125);
      #endif // NORM_FARST

      // extract channels of certain patch
      vector<Mat> patch_channels;
      patch_channels.push_back(input_channels[i][3 * j + 0]);	// B
      patch_channels.push_back(input_channels[i][3 * j + 1]);	// G
      patch_channels.push_back(input_channels[i][3 * j + 2]);	// R
      split(crop, patch_channels);
    }
  }

  const vector<caffe::Blob<float>*> out = Lnet->Forward();

  // for every facial landmark
  for (int j = 0; j < 5; ++j)
  {
    caffe::Blob<float>* offs = out[j];
    // for every face
    for (int i = 0; i < num; ++i)
    {
      int patch_size = patch_sizes.at<int>(i,j);
      float off_x = offs->data_at(i, 0, 0, 0) - 0.5;
      float off_y = offs->data_at(i, 1, 0, 0) - 0.5;
      // Dot not make large movement with relative offset > 0.35
      if (fabs(off_x) <= 0.35 && fabs(off_y) <= 0.35)
      {
        infos[i].fpts[j].x += off_x * patch_size;
        infos[i].fpts[j].y += off_y * patch_size;
      }
    }
  }
}
#endif // MTCNN_PRECISE_LANDMARK