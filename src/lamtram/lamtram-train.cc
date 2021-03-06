
#include <lamtram/lamtram-train.h>
#include <lamtram/neural-lm.h>
#include <lamtram/encoder-decoder.h>
#include <lamtram/encoder-attentional.h>
#include <lamtram/encoder-classifier.h>
#include <lamtram/macros.h>
#include <lamtram/timer.h>
#include <lamtram/model-utils.h>
#include <lamtram/string-util.h>
#include <lamtram/loss-stats.h>
#include <lamtram/eval-measure.h>
#include <lamtram/eval-measure-loader.h>
#include <dynet/dynet.h>
#include <dynet/dict.h>
#include <dynet/globals.h>
#include <dynet/training.h>
#include <dynet/tensor.h>
#include <boost/program_options.hpp>
#include <boost/range/irange.hpp>
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <fstream>
#include <string>

using namespace std;
using namespace lamtram;
using namespace dynet::expr;
namespace po = boost::program_options;

int LamtramTrain::main(int argc, char** argv) {
  po::options_description desc("*** lamtram-train (by Graham Neubig) ***");
  desc.add_options()
    ("help", "Produce help message")
    ("train_trg", po::value<string>()->default_value(""), "Training files, possibly separated by pipes")
    ("dev_trg", po::value<string>()->default_value(""), "Development files")
    ("train_src", po::value<string>()->default_value(""), "Training source files for TMs, possibly separated by pipes")
    ("dev_src", po::value<string>()->default_value(""), "Development source file for TMs")
    ("model_out", po::value<string>()->default_value(""), "File to write the model to")
    ("model_type", po::value<string>()->default_value("nlm"), "Model type (Neural LM nlm, Encoder Decoder encdec, Attentional Model encatt, or Encoder Classifier enccls)")
    ("layer_size", po::value<int>()->default_value(512), "The default size of all hidden layers (word rep, hidden state, mlp attention, mlp softmax) if not specified otherwise")
    ("attention_feed", po::value<bool>()->default_value(true), "Whether to perform the input feeding of Luong et al.")
    ("attention_hist", po::value<string>()->default_value("none"), "How to pass information about the attention into the score function (none/sum)")
    ("attention_lex", po::value<string>()->default_value("none"), "Use a lexicon (e.g. \"prior:file=/path/to/file:alpha=0.001\")")
    ("attention_type", po::value<string>()->default_value("mlp:0"), "Type of attention score (mlp:NUM/bilin/dot)")
    ("cls_layers", po::value<string>()->default_value(""), "Descriptor for classifier layers, nodes1:nodes2:...")
    ("context", po::value<int>()->default_value(2), "Amount of context information to use")
    ("dropout", po::value<float>()->default_value(0.0), "Dropout rate during training")
    ("encoder_types", po::value<string>()->default_value("for|rev"), "The type of encoder, multiple separated by a pipe (for=forward, rev=reverse)")
    ("epochs", po::value<int>()->default_value(100), "Number of epochs")
    ("eval_every", po::value<int>()->default_value(-1), "Evaluate every n sentences (-1 for full training set)")
    ("early_stop", po::value<int>()->default_value(-1), "Stop if no improvement in n evals (TMs only, -1 for no early stopping)")
    ("eval_meas", po::value<string>()->default_value("bleu:smooth=1"), "The evaluation measure to use for minimum risk training (default: BLEU+1)")
    ("layers", po::value<string>()->default_value("lstm:0:1"), "Descriptor for hidden layers, type:num_units:num_layers")
    ("learning_criterion", po::value<string>()->default_value("ml"), "The criterion to use for learning (ml/minrisk)")
    ("learning_rate", po::value<float>()->default_value(0.001), "Learning rate")
    ("minibatch_size", po::value<int>()->default_value(1), "Number of words per mini-batch")
    ("minrisk_dedup", po::value<bool>()->default_value(true), "Whether to deduplicate samples for min risk training")
    ("minrisk_include_ref", po::value<bool>()->default_value(false), "Whether to include the reference in every sample for min risk training")
    ("minrisk_max_len", po::value<int>()->default_value(200), "Limit on the max length of sentences")
    ("minrisk_num_samples", po::value<int>()->default_value(50), "The number of samples to perform for minimum risk training")
    ("minrisk_scaling", po::value<float>()->default_value(0.005), "The scaling factor for min risk training")
    ("model_in", po::value<string>()->default_value(""), "If resuming training, read the model in")
    ("rate_decay", po::value<float>()->default_value(0.5), "Learning rate decay when dev perplexity gets worse")
    ("rate_thresh",  po::value<float>()->default_value(1e-5), "Threshold for the learning rate")
    ("scheduled_samp", po::value<float>()->default_value(0.f), "If set to 1 or more, perform scheduled sampling where the selected value is the number of iterations after which the sampling value reaches 0.5")
    ("seed", po::value<int>()->default_value(0), "Random seed (default 0 -> changes every time)")
    ("softmax", po::value<string>()->default_value("multilayer:0:full"), "The type of softmax to use (full/hinge/hier/mod/multilayer) see softmax_factory.h for details")
    ("train_weights", po::value<string>()->default_value(""), "Training instance weights for TMs, possibly separated by pipes")
    ("train_kickout_keep", po::value<string>()->default_value(""), "Instance-level keep rates for kickout (TMs only), possibly separated by pipes")
    ("trainer", po::value<string>()->default_value("adam"), "Training algorithm (sgd/momentum/adagrad/adadelta)")
    ("verbose", po::value<int>()->default_value(0), "How much verbose output to print")
    ("wildcards", po::value<string>()->default_value(""), "Wildcards to be used in loading training files")
    ("wordrep", po::value<int>()->default_value(0), "Size of the word representations (0 to match layer_size)")
    ;
  po::store(po::parse_command_line(argc, argv, desc), vm_);
  po::notify(vm_);   
  if (vm_.count("help")) {
    cout << desc << endl;
    return 1;
  }
  for(int i = 0; i < argc; i++) { cerr << argv[i] << " "; } cerr << endl;

  GlobalVars::verbose = vm_["verbose"].as<int>();
  GlobalVars::layer_size = vm_["layer_size"].as<int>();

  // Set random seed if necessary
  int seed = vm_["seed"].as<int>();
  if(seed != 0) {
    delete dynet::rndeng;
    dynet::rndeng = new mt19937(seed);
  }

  // Sanity check for model type
  string model_type = vm_["model_type"].as<std::string>();
  if(model_type != "nlm" && model_type != "encdec" && model_type != "encatt" && model_type != "enccls") {
    cerr << desc << endl;
    THROW_ERROR("Model type must be neural LM (nlm) encoder decoder (encdec), attentional model (encatt), or encoder classifier (enccls)");
  }
  bool use_src = model_type == "encdec" || model_type == "enccls" || model_type == "encatt";

  // Create the wildcards
  wildcards_ = Tokenize(vm_["wildcards"].as<string>(), "|");

  // Other sanity checks
  try { train_files_trg_ = TokenizeWildcarded(vm_["train_trg"].as<string>(), wildcards_, "|"); } catch(std::exception & e) { }
  try { dev_file_trg_ = vm_["dev_trg"].as<string>(); } catch(std::exception & e) { }
  try { model_out_file_ = vm_["model_out"].as<string>(); } catch(std::exception & e) { }
  if(!train_files_trg_.size())
    THROW_ERROR("Must specify a training file with --train_trg");
  if(!model_out_file_.size())
    THROW_ERROR("Must specify a model output file with --model_out");

  // Sanity checks for the source
  try { train_files_src_ = TokenizeWildcarded(vm_["train_src"].as<string>(), wildcards_, "|"); } catch(std::exception & e) { }
  try { dev_file_src_ = vm_["dev_src"].as<string>(); } catch(std::exception & e) { }
  try {
    string train_weights_string = vm_["train_weights"].as<string>();
    if (train_weights_string != "")
      train_files_weights_ = TokenizeWildcarded(vm_["train_weights"].as<string>(), wildcards_, "|");
  } catch(std::exception & e) { }
  try {
    string train_kickout_keep_string = vm_["train_kickout_keep"].as<string>();
    if (train_kickout_keep_string != "")
      train_files_kickout_keep_ = TokenizeWildcarded(train_kickout_keep_string, wildcards_, "|");
  } catch(std::exception & e) { }
  if(use_src && ((!train_files_src_.size()) || (dev_file_trg_.size() && !dev_file_src_.size())))
    THROW_ERROR("The specified model requires a source file to train, specify source files using train_src.");

  // Save some variables
  rate_decay_ = vm_["rate_decay"].as<float>();
  rate_thresh_ = vm_["rate_thresh"].as<float>();
  epochs_ = vm_["epochs"].as<int>();
  context_ = vm_["context"].as<int>();
  model_in_file_ = vm_["model_in"].as<string>();
  model_out_file_ = vm_["model_out"].as<string>();
  eval_every_ = vm_["eval_every"].as<int>();
  softmax_sig_ = vm_["softmax"].as<string>();
  scheduled_samp_ = vm_["scheduled_samp"].as<float>();
  dropout_ = vm_["dropout"].as<float>();

  // Perform appropriate training
  if(model_type == "nlm")           TrainLM();
  else if(model_type == "encdec")   TrainEncDec();
  else if(model_type == "encatt")   TrainEncAtt();
  else if(model_type == "enccls")   TrainEncCls();
  else                THROW_ERROR("Bad model type " << model_type);

  return 0;
}

template <class OutputType>
struct DoubleLength
{
  DoubleLength(const vector<Sentence> & v, const vector<OutputType> & w) : vec(v), wec(w) { }
  bool operator() (int i1, int i2);
  const vector<Sentence> & vec;
  const vector<OutputType> & wec;
};

template <>
bool DoubleLength<Sentence>::operator() (int i1, int i2) {
  if(vec[i2].size() != vec[i1].size()) return (vec[i2].size() < vec[i1].size());
  return (wec[i2].size() < wec[i1].size());
}

template <>
bool DoubleLength<int>::operator() (int i1, int i2) {
  return (vec[i2].size() < vec[i1].size());
}

struct SingleLength
{
  SingleLength(const vector<Sentence> & v) : vec(v) { }
  inline bool operator() (int i1, int i2)
  {
    return (vec[i2].size() < vec[i1].size());
  }
  const vector<Sentence> & vec;
};

inline size_t CalcSize(const Sentence & src, const Sentence & trg) {
  return src.size()+trg.size();
}
inline size_t CalcSize(const Sentence & src, int trg) {
  return src.size()+1;
}

template <class OutputType>
inline size_t CreateMinibatches(const std::vector<Sentence> & train_src,
                              const std::vector<OutputType> & train_trg,
                              const std::vector<OutputType> & train_cache,
                              const std::vector<float> & train_weights,
                              const std::vector<float> & train_kickout_keep,
                              size_t max_size,
                              std::vector<std::vector<Sentence> > & train_src_minibatch,
                              std::vector<std::vector<OutputType> > & train_trg_minibatch,
                              std::vector<std::vector<OutputType> > & train_cache_minibatch,
                              std::vector<std::vector<float> > & train_weights_minibatch,
                              std::vector<size_t> & train_ids_minibatch) {
  train_src_minibatch.clear();
  train_trg_minibatch.clear();
  train_cache_minibatch.clear();
  train_weights_minibatch.clear();
  std::vector<size_t> train_ids(train_trg.size());
  std::iota(train_ids.begin(), train_ids.end(), 0);
  if(max_size > 1)
    sort(train_ids.begin(), train_ids.end(), DoubleLength<OutputType>(train_src, train_trg));
  std::vector<Sentence> train_src_next;
  std::vector<OutputType> train_trg_next, train_cache_next;
  std::vector<float> train_weights_next;
  size_t max_len = 0;
  size_t kicked = 0;
  for(size_t i = 0; i < train_ids.size(); i++) {
    // Apply kickout: skip sentence if rand [0,1] above keep rate
    if(train_kickout_keep.size()) {
      if(dynet::rand01() > train_kickout_keep[train_ids[i]]) {
        ++kicked;
        continue;
      }
    }
    max_len = max(max_len, CalcSize(train_src[train_ids[i]], train_trg[train_ids[i]]));
    train_src_next.push_back(train_src[train_ids[i]]);
    train_trg_next.push_back(train_trg[train_ids[i]]);
    if(train_cache.size())
      train_cache_next.push_back(train_cache[train_ids[i]]);
    if(train_weights.size())
      train_weights_next.push_back(train_weights[train_ids[i]]);
    if((train_trg_next.size()+1) * max_len > max_size) {
      train_src_minibatch.push_back(train_src_next);
      train_src_next.clear();
      train_trg_minibatch.push_back(train_trg_next);
      train_trg_next.clear();
      if(train_cache.size()) {
        train_cache_minibatch.push_back(train_cache_next);
        train_cache_next.clear();
      }
      if(train_weights.size()) {
        train_weights_minibatch.push_back(train_weights_next);
        train_weights_next.clear();
      }
      max_len = 0;
    }
  }
  if(train_trg_next.size()) {
    train_src_minibatch.push_back(train_src_next);
    train_trg_minibatch.push_back(train_trg_next);
  }
  if(train_cache_next.size()) train_cache_minibatch.push_back(train_cache_next);
  if(train_weights_next.size()) train_weights_minibatch.push_back(train_weights_next);
  // Create a sentence list for this minibatch
  train_ids_minibatch.resize(train_src_minibatch.size());
  std::iota(train_ids_minibatch.begin(), train_ids_minibatch.end(), 0);
  // Return total size (sentences)
  if(train_kickout_keep.size()) {
    cerr << "*** Kickout: " << train_ids.size() - kicked << " of " << train_ids.size() << " instances retained" << endl;
    return train_ids.size() - kicked;
  }
  return train_ids.size();
}

inline void CreateMinibatches(const std::vector<Sentence> & train_trg,
                              const std::vector<Sentence> & train_cache,
                              int max_size,
                              std::vector<std::vector<Sentence> > & train_trg_minibatch,
                              std::vector<std::vector<Sentence> > & train_cache_minibatch) {
  std::vector<int> train_ids(train_trg.size());
  std::iota(train_ids.begin(), train_ids.end(), 0);
  if(max_size > 1)
    sort(train_ids.begin(), train_ids.end(), SingleLength(train_trg));
  std::vector<Sentence> train_trg_next, train_cache_next;
  size_t first_size = 0;
  for(size_t i = 0; i < train_ids.size(); i++) {
    if(train_trg_next.size() == 0)
      first_size = train_trg[train_ids[i]].size();
    train_trg_next.push_back(train_trg[train_ids[i]]);
    if(train_cache.size())
      train_cache_next.push_back(train_cache[train_ids[i]]);
    if((train_trg_next.size()+1) * first_size > max_size) {
      train_trg_minibatch.push_back(train_trg_next);
      train_trg_next.clear();
      if(train_cache.size()) {
        train_cache_minibatch.push_back(train_cache_next);
        train_cache_next.clear();
      }
    }
  }
  if(train_trg_next.size())   train_trg_minibatch.push_back(train_trg_next);
  if(train_cache_next.size()) train_cache_minibatch.push_back(train_cache_next);
}

void LamtramTrain::TrainLM() {

  // dynet::Dict
  DictPtr vocab_trg, vocab_src;
  std::shared_ptr<dynet::Model> model;
  std::shared_ptr<NeuralLM> nlm;
  if(model_in_file_.size()) {
    nlm.reset(ModelUtils::LoadMonolingualModel<NeuralLM>(model_in_file_, model, vocab_trg));
  } else {
    vocab_trg.reset(CreateNewDict());
    model.reset(new dynet::Model);
  }
  // if(!trg_sent) vocab_trg = dynet::Dict("");

  // Read the training files
  vector<Sentence> train_trg, dev_trg, train_cache;
  vector<int> train_trg_ids;
  for(size_t i = 0; i < train_files_trg_.size(); i++) {
    LoadFile(train_files_trg_[i], true, *vocab_trg, train_trg);
    train_trg_ids.resize(train_trg.size(), i);
  }
  if(!vocab_trg->is_frozen()) { vocab_trg->freeze(); vocab_trg->set_unk("<unk>"); }
  if(dev_file_trg_.size()) LoadFile(dev_file_trg_, true, *vocab_trg, dev_trg);
  if(train_files_weights_.size())
    THROW_ERROR("Instance weighting only supported for encdec and encatt models")
  if(train_files_kickout_keep_.size())
    THROW_ERROR("Kickout only supported for encdec and encatt models")
  if(eval_every_ == -1) eval_every_ = train_trg.size();

  // Create the model
  int wordrep = vm_["wordrep"].as<int>();
  if(wordrep <= 0) wordrep = GlobalVars::layer_size;
  if(model_in_file_.size() == 0)
    nlm.reset(new NeuralLM(vocab_trg, context_, 0, false, wordrep, vm_["layers"].as<string>(), vocab_trg->get_unk_id(), softmax_sig_, *model));
  TrainerPtr trainer = GetTrainer(vm_["trainer"].as<string>(), vm_["learning_rate"].as<float>(), *model);

  // If necessary, cache the softmax
  nlm->GetSoftmax().Cache(train_trg, train_trg_ids, train_cache);

  // Create minibatches
  vector<vector<Sentence> > train_trg_minibatch, train_cache_minibatch, dev_trg_minibatch, dev_cache_minibatch;
  vector<Sentence> empty_minibatch;
  CreateMinibatches(train_trg, train_cache, vm_["minibatch_size"].as<int>(), train_trg_minibatch, train_cache_minibatch);
  // CreateMinibatches(dev_trg, empty_minibatch, vm_["minibatch_size"].as<int>(), dev_trg_minibatch, dev_cache_minibatch);
  CreateMinibatches(dev_trg, empty_minibatch, 1, dev_trg_minibatch, dev_cache_minibatch);
  
  // TODO: Learning rate
  dynet::real learning_rate = vm_["learning_rate"].as<float>();
  dynet::real learning_scale = 1.0;

  // Create a sentence list and random generator
  std::vector<int> train_ids(train_trg_minibatch.size());
  std::iota(train_ids.begin(), train_ids.end(), 0);
  // Perform the training
  std::vector<dynet::expr::Expression> empty_hist;
  dynet::real last_loss = 1e99, best_loss = 1e99;
  bool is_likelihood = (softmax_sig_ != "hinge");
  bool do_dev = dev_trg.size() != 0;
  int loc = 0, sent_loc = 0, last_print = 0;
  float epoch_frac = 0.f, samp_prob = 0.f;
  int epoch = 0;
  std::shuffle(train_ids.begin(), train_ids.end(), *dynet::rndeng);
  while(true) {
    // Start the training
    LLStats train_ll(nlm->GetVocabSize()), dev_ll(nlm->GetVocabSize());
    train_ll.is_likelihood_ = is_likelihood; dev_ll.is_likelihood_ = is_likelihood;
    Timer time;
    nlm->SetDropout(dropout_);
    for(int curr_sent_loc = 0; curr_sent_loc < eval_every_; ) {
      if(loc == (int)train_ids.size()) {
        // Shuffle the access order
        std::shuffle(train_ids.begin(), train_ids.end(), *dynet::rndeng);
        loc = 0;
        sent_loc = 0;
        last_print = 0;
        ++epoch;
        if(epoch >= epochs_) return;
      }
      if(scheduled_samp_) {
        float val = (epoch_frac-scheduled_samp_)/scheduled_samp_;
        samp_prob = 1/(1+exp(val));
      }
      dynet::ComputationGraph cg;
      nlm->NewGraph(cg);
      dynet::expr::Expression loss_exp = nlm->BuildSentGraph(train_trg_minibatch[train_ids[loc]], (train_cache_minibatch.size() ? train_cache_minibatch[train_ids[loc]] : empty_minibatch), nullptr, NULL, empty_hist, samp_prob, true, cg, train_ll);
      sent_loc += train_trg_minibatch[train_ids[loc]].size();
      curr_sent_loc += train_trg_minibatch[train_ids[loc]].size();
      epoch_frac += 1.f/train_ids.size();
      // cg.PrintGraphviz();
      train_ll.loss_ += as_scalar(cg.incremental_forward(loss_exp));
      cg.backward(loss_exp);
      trainer->update();
      ++loc;
      if(sent_loc / 100 != last_print || curr_sent_loc >= eval_every_ || epochs_ == epoch) {
        last_print = sent_loc / 100;
        float elapsed = time.Elapsed();
        cerr << "Epoch " << epoch+1 << " sent " << sent_loc << ": " << train_ll.PrintStats() << ", rate=" << learning_scale*learning_rate << ", time=" << elapsed << " (" << train_ll.words_/elapsed << " w/s)" << endl;
        if(epochs_ == epoch) break;
      }
    }
    // Measure development perplexity
    if(do_dev) {
      time = Timer();
      nlm->SetDropout(0.f);
      for(auto & sent : dev_trg_minibatch) {
        dynet::ComputationGraph cg;
        nlm->NewGraph(cg);
        dynet::expr::Expression loss_exp = nlm->BuildSentGraph(sent, empty_minibatch, nullptr, NULL, empty_hist, 0.f, false, cg, dev_ll);
        dev_ll.loss_ += as_scalar(cg.incremental_forward(loss_exp));
      }
      float elapsed = time.Elapsed();
      cerr << "Epoch " << epoch+1 << " dev: " << dev_ll.PrintStats() << ", rate=" << learning_scale*learning_rate << ", time=" << elapsed << " (" << dev_ll.words_/elapsed << " w/s)" << endl;
    }
    // Adjust the learning rate
    trainer->update_epoch();
    // trainer->status(); cerr << endl;
    // Check the learning rate
    if(last_loss != last_loss)
      THROW_ERROR("Likelihood is not a number, dying...");
    dynet::real my_loss = do_dev ? dev_ll.loss_ : train_ll.loss_;
    if(my_loss > last_loss) {
      learning_scale *= rate_decay_;
    }
    last_loss = my_loss;
    if(best_loss > my_loss) {
      // Open the output stream
      ofstream out(model_out_file_.c_str());
      if(!out) THROW_ERROR("Could not open output file: " << model_out_file_);
      cerr << "*** Found the best model yet! Printing model to " << model_out_file_ << endl;
      // Write the model (TODO: move this to a separate file?)
      WriteDict(*vocab_trg, out);
      // vocab_trg->Write(out);
      nlm->Write(out);
      ModelUtils::WriteModelText(out, *model);
      best_loss = my_loss;
    }
    // If the rate is less than the threshold
    if(learning_scale*learning_rate < rate_thresh_)
      break;
  }
}

void LamtramTrain::TrainEncDec() {

  // dynet::Dict
  DictPtr vocab_trg, vocab_src;
  std::shared_ptr<dynet::Model> model;
  std::shared_ptr<EncoderDecoder> encdec;
  NeuralLMPtr decoder;
  if(model_in_file_.size()) {
    encdec.reset(ModelUtils::LoadBilingualModel<EncoderDecoder>(model_in_file_, model, vocab_src, vocab_trg));
    decoder = encdec->GetDecoderPtr();
  } else {
    vocab_src.reset(CreateNewDict());
    vocab_trg.reset(CreateNewDict());
    model.reset(new dynet::Model);
  }
  // if(!trg_sent) vocab_trg = dynet::Dict("");

  // Read the training files
  vector<Sentence> train_trg, dev_trg, train_src, dev_src, train_cache_ids;
  vector<int> train_trg_ids, train_src_ids;
  vector<float> train_weights, train_kickout_keep;
  for(size_t i = 0; i < train_files_trg_.size(); i++) {
    LoadFile(train_files_trg_[i], true, *vocab_trg, train_trg);
    train_trg_ids.resize(train_trg.size(), i);
  }
  if(!vocab_trg->is_frozen()) { vocab_trg->freeze(); vocab_trg->set_unk("<unk>"); }
  if(dev_file_trg_.size()) LoadFile(dev_file_trg_, true, *vocab_trg, dev_trg);
  for(size_t i = 0; i < train_files_src_.size(); i++) {
    LoadFile(train_files_src_[i], false, *vocab_src, train_src);
    train_src_ids.resize(train_src.size(), i);
  }
  if(!vocab_src->is_frozen()) { vocab_src->freeze(); vocab_src->set_unk("<unk>"); }
  if(dev_file_src_.size()) LoadFile(dev_file_src_, false, *vocab_src, dev_src);
  for(size_t i = 0; i < train_files_weights_.size(); i++)
    LoadWeights(train_files_weights_[i], train_weights);
  for(size_t i = 0; i < train_files_kickout_keep_.size(); i++)
    LoadWeights(train_files_kickout_keep_[i], train_kickout_keep);

  // Create the model
  if(model_in_file_.size() == 0) {
    vector<LinearEncoderPtr> encoders;
    vector<string> encoder_types;
    boost::algorithm::split(encoder_types, vm_["encoder_types"].as<string>(), boost::is_any_of("|"));
    BuilderSpec dec_layer_spec(vm_["layers"].as<string>());
    if(dec_layer_spec.nodes % encoder_types.size() != 0)
      THROW_ERROR("The number of nodes in the decoder (" << dec_layer_spec.nodes << ") must be divisible by the number of encoders (" << encoder_types.size() << ")");
    BuilderSpec enc_layer_spec(dec_layer_spec); enc_layer_spec.nodes /= encoder_types.size();
    int wordrep = vm_["wordrep"].as<int>();
    if(wordrep <= 0) wordrep = GlobalVars::layer_size;
    for(auto & spec : encoder_types) {
      LinearEncoderPtr enc(new LinearEncoder(vocab_src->size(), wordrep, enc_layer_spec, vocab_src->get_unk_id(), *model));
      if(spec == "for") { }
      else if(spec == "rev") { enc->SetReverse(true); }
      else { THROW_ERROR("Illegal encoder type: " << spec); }
      encoders.push_back(enc);
    }
    decoder.reset(new NeuralLM(vocab_trg, context_, 0, false, wordrep, dec_layer_spec, vocab_trg->get_unk_id(), softmax_sig_, *model));
    encdec.reset(new EncoderDecoder(encoders, decoder, *model));
  }

  string crit = vm_["learning_criterion"].as<string>();
  if(crit == "ml") {
    // If necessary, cache the softmax
    decoder->GetSoftmax().Cache(train_trg, train_trg_ids, train_cache_ids);
    BilingualTraining(train_src,
                      train_trg,
                      train_cache_ids,
                      train_weights,
                      train_kickout_keep,
                      dev_src,
                      dev_trg,
                      *vocab_src,
                      *vocab_trg,
                      *model,
                      *encdec);
  } else if(crit == "minrisk") {
    // Get the evaluator
    std::shared_ptr<EvalMeasure> eval(EvalMeasureLoader::CreateMeasureFromString(vm_["eval_meas"].as<string>(), *vocab_trg));
    MinRiskTraining(train_src, train_trg, train_trg_ids, dev_src, dev_trg,
                    *vocab_src, *vocab_trg, *eval, *model, *encdec);
  } else {
    THROW_ERROR("Illegal learning criterion: " << crit);
  }
}

void LamtramTrain::TrainEncAtt() {

  // dynet::Dict
  DictPtr vocab_trg, vocab_src;
  std::shared_ptr<dynet::Model> model;
  std::shared_ptr<EncoderAttentional> encatt;
  NeuralLMPtr decoder;
  if(model_in_file_.size()) {
    encatt.reset(ModelUtils::LoadBilingualModel<EncoderAttentional>(model_in_file_, model, vocab_src, vocab_trg));
    decoder = encatt->GetDecoderPtr();
  } else {
    vocab_src.reset(CreateNewDict());
    vocab_trg.reset(CreateNewDict());
    model.reset(new dynet::Model);
  }

  // Read the training file
  vector<Sentence> train_trg, dev_trg, train_src, dev_src, train_cache_ids;
  vector<int> train_trg_ids, train_src_ids;
  vector<float> train_weights, train_kickout_keep;
  for(size_t i = 0; i < train_files_trg_.size(); i++) {
    LoadFile(train_files_trg_[i], true, *vocab_trg, train_trg);
    train_trg_ids.resize(train_trg.size(), i);
  }
  if(!vocab_trg->is_frozen()) { vocab_trg->freeze(); vocab_trg->set_unk("<unk>"); }
  if(dev_file_trg_.size()) LoadFile(dev_file_trg_, true, *vocab_trg, dev_trg);
  for(size_t i = 0; i < train_files_src_.size(); i++) {
    LoadFile(train_files_src_[i], false, *vocab_src, train_src);
    train_src_ids.resize(train_src.size(), i);
  }
  if(!vocab_src->is_frozen()) { vocab_src->freeze(); vocab_src->set_unk("<unk>"); }
  if(dev_file_src_.size()) LoadFile(dev_file_src_, false, *vocab_src, dev_src);
  for(size_t i = 0; i < train_files_weights_.size(); i++)
    LoadWeights(train_files_weights_[i], train_weights);
  for(size_t i = 0; i < train_files_kickout_keep_.size(); i++)
    LoadWeights(train_files_kickout_keep_[i], train_kickout_keep);

  // Create the model
  if(model_in_file_.size() == 0) {
    vector<LinearEncoderPtr> encoders;
    vector<string> encoder_types;
    boost::algorithm::split(encoder_types, vm_["encoder_types"].as<string>(), boost::is_any_of("|"));
    BuilderSpec dec_layer_spec(vm_["layers"].as<string>());
    if(dec_layer_spec.nodes % encoder_types.size() != 0)
      THROW_ERROR("The number of nodes in the decoder (" << dec_layer_spec.nodes << ") must be divisible by the number of encoders (" << encoder_types.size() << ")");
    BuilderSpec enc_layer_spec(dec_layer_spec); enc_layer_spec.nodes /= encoder_types.size();
    int wordrep = vm_["wordrep"].as<int>();
    if(wordrep <= 0) wordrep = GlobalVars::layer_size;
    for(auto & spec : encoder_types) {
      LinearEncoderPtr enc(new LinearEncoder(vocab_src->size(), wordrep, enc_layer_spec, vocab_src->get_unk_id(), *model));
      if(spec == "rev") enc->SetReverse(true);
      encoders.push_back(enc);
    }
    ExternAttentionalPtr extatt(new ExternAttentional(encoders, vm_["attention_type"].as<string>(), vm_["attention_hist"].as<string>(), dec_layer_spec.nodes, vm_["attention_lex"].as<string>(), vocab_src, vocab_trg, *model));
    decoder.reset(new NeuralLM(vocab_trg, context_, dec_layer_spec.nodes, vm_["attention_feed"].as<bool>(), wordrep, dec_layer_spec, vocab_trg->get_unk_id(), softmax_sig_, *model));
    encatt.reset(new EncoderAttentional(extatt, decoder, *model));
  }

  string crit = vm_["learning_criterion"].as<string>();
  if(crit == "ml") {
    // If necessary, cache the softmax
    decoder->GetSoftmax().Cache(train_trg, train_trg_ids, train_cache_ids);
    BilingualTraining(train_src,
                      train_trg,
                      train_cache_ids,
                      train_weights,
                      train_kickout_keep,
                      dev_src,
                      dev_trg,
                      *vocab_src,
                      *vocab_trg,
                      *model,
                      *encatt);
  } else if(crit == "minrisk") {
    // Get the evaluator
    std::shared_ptr<EvalMeasure> eval(EvalMeasureLoader::CreateMeasureFromString(vm_["eval_meas"].as<string>(), *vocab_trg));
    MinRiskTraining(train_src, train_trg, train_trg_ids, dev_src, dev_trg,
                    *vocab_src, *vocab_trg, *eval, *model, *encatt);
  } else {
    THROW_ERROR("Illegal learning criterion: " << crit);
  }
}

void LamtramTrain::TrainEncCls() {

  // dynet::Dict
  DictPtr vocab_trg, vocab_src;
  std::shared_ptr<dynet::Model> model;
  std::shared_ptr<EncoderClassifier> enccls;
  if(model_in_file_.size()) {
    enccls.reset(ModelUtils::LoadBilingualModel<EncoderClassifier>(model_in_file_, model, vocab_src, vocab_trg));
  } else {
    vocab_src.reset(CreateNewDict());
    vocab_trg.reset(CreateNewDict(false));
    model.reset(new dynet::Model);
  }
  // if(!trg_sent) vocab_trg = dynet::Dict("");

  // Read the training file
  vector<Sentence> train_src, dev_src;
  vector<int> train_trg, dev_trg;
  vector<int> train_trg_ids, train_src_ids;
  vector<float> train_weights, train_kickout_keep;
  for(size_t i = 0; i < train_files_trg_.size(); i++) {
    LoadLabels(train_files_trg_[i], *vocab_trg, train_trg);
    train_trg_ids.resize(train_trg.size(), i);
  }
  vocab_trg->freeze();
  if(dev_file_trg_.size()) LoadLabels(dev_file_trg_, *vocab_trg, dev_trg);
  for(size_t i = 0; i < train_files_src_.size(); i++) {
    LoadFile(train_files_src_[i], false, *vocab_src, train_src);
    train_src_ids.resize(train_src.size(), i);
  }
  if(!vocab_src->is_frozen()) { vocab_src->freeze(); vocab_src->set_unk("<unk>"); }
  if(dev_file_src_.size()) LoadFile(dev_file_src_, false, *vocab_src, dev_src);
  if(train_files_weights_.size())
    THROW_ERROR("Instance weighting only supported for encdec and encatt models")
  if(train_files_kickout_keep_.size())
    THROW_ERROR("Kickout only supported for encdec and encatt models")

  // Create the model
  if(model_in_file_.size() == 0) {
    vector<LinearEncoderPtr> encoders;
    vector<string> encoder_types;
    boost::algorithm::split(encoder_types, vm_["encoder_types"].as<string>(), boost::is_any_of("|"));
    int wordrep = vm_["wordrep"].as<int>();
    if(wordrep <= 0) wordrep = GlobalVars::layer_size;
    for(auto & spec : encoder_types) {
      LinearEncoderPtr enc(new LinearEncoder(vocab_src->size(), wordrep, vm_["layers"].as<string>(), vocab_src->get_unk_id(), *model));
      if(spec == "rev") enc->SetReverse(true);
      encoders.push_back(enc);
    }
    BuilderSpec bspec(vm_["layers"].as<string>());
    ClassifierPtr classifier(new Classifier(bspec.nodes * encoders.size(), vocab_trg->size(), vm_["cls_layers"].as<string>(), vm_["softmax"].as<string>(), *model));
    enccls.reset(new EncoderClassifier(encoders, classifier, *model));
  }

  vector<int> train_cache_ids(train_trg.size());
  BilingualTraining(train_src,
                    train_trg,
                    train_cache_ids,
                    train_weights,
                    train_kickout_keep,
                    dev_src,
                    dev_trg,
                    *vocab_src,
                    *vocab_trg,
                    *model,
                    *enccls);
}

template<class ModelType, class OutputType>
void LamtramTrain::BilingualTraining(const vector<Sentence> & train_src,
                                     const vector<OutputType> & train_trg,
                                     const vector<OutputType> & train_cache,
                                     const vector<float> & train_weights,
                                     const vector<float> & train_kickout_keep,
                                     const vector<Sentence> & dev_src,
                                     const vector<OutputType> & dev_trg,
                                     const dynet::Dict & vocab_src,
                                     const dynet::Dict & vocab_trg,
                                     dynet::Model & model,
                                     ModelType & encdec) {

  // Sanity checks
  assert(train_src.size() == train_trg.size());
  assert(!train_weights.size() || train_weights.size() == train_trg.size());
  assert(!train_kickout_keep.size() || train_kickout_keep.size() == train_trg.size());
  assert(dev_src.size() == dev_trg.size());

  // Create minibatches
  vector<vector<Sentence> > train_src_minibatch, dev_src_minibatch;
  vector<vector<OutputType> > train_trg_minibatch, train_cache_minibatch, dev_trg_minibatch, dev_cache_minibatch;
  vector<vector<float> > train_weights_minibatch, dev_weights_minibatch;
  vector<size_t> train_ids_minibatch, dev_ids_minibatch;
  vector<float> dev_weights; // For now, use empty vector to indicate uniform weights for dev set
  vector<float> dev_kickout_keep; // For now, use empty vector to indicate no kickout for dev set
  vector<Sentence> empty_minibatch;
  std::vector<OutputType> empty_cache;
  size_t minibatch_size = vm_["minibatch_size"].as<int>();
  size_t train_instances = CreateMinibatches(train_src,
                                             train_trg,
                                             train_cache,
                                             train_weights,
                                             train_kickout_keep,
                                             minibatch_size,
                                             train_src_minibatch,
                                             train_trg_minibatch,
                                             train_cache_minibatch,
                                             train_weights_minibatch,
                                             train_ids_minibatch);
  if(vm_["eval_every"].as<int>() == -1) eval_every_ = train_instances;
  CreateMinibatches(dev_src,
                    dev_trg,
                    empty_cache,
                    dev_weights,
                    dev_kickout_keep,
                    1,
                    dev_src_minibatch,
                    dev_trg_minibatch,
                    dev_cache_minibatch,
                    dev_weights_minibatch,
                    dev_ids_minibatch);
  TrainerPtr trainer = GetTrainer(vm_["trainer"].as<string>(), vm_["learning_rate"].as<float>(), model);
  
  // Learning rate
  dynet::real learning_rate = vm_["learning_rate"].as<float>();
  dynet::real learning_scale = 1.0;
  // Early stopping
  int evals_since_improvement = 0;
  int early_stop = vm_["early_stop"].as<int>();

  // Perform the training
  std::vector<dynet::expr::Expression> empty_hist;
  dynet::real last_loss = 1e99, best_loss = 1e99;
  bool is_likelihood = (softmax_sig_ != "hinge");
  bool do_dev = dev_src.size() != 0;
  int loc = 0, epoch = 0, sent_loc = 0, last_print = 0;
  float epoch_frac = 0.f, samp_prob = 0.f;
  // Shuffle minibatches
  std::shuffle(train_ids_minibatch.begin(), train_ids_minibatch.end(), *dynet::rndeng);
  while(true) {
    // Start the training
    LLStats train_ll(vocab_trg.size()), dev_ll(vocab_trg.size());
    train_ll.is_likelihood_ = is_likelihood; dev_ll.is_likelihood_ = is_likelihood;
    Timer time;
    encdec.SetDropout(dropout_);
    for(int curr_sent_loc = 0; curr_sent_loc < eval_every_; ) {
      if(loc == (int)train_ids_minibatch.size()) {
        if(train_kickout_keep.size()) {
          train_instances = CreateMinibatches(train_src,
                                              train_trg,
                                              train_cache,
                                              train_weights,
                                              train_kickout_keep,
                                              minibatch_size,
                                              train_src_minibatch,
                                              train_trg_minibatch,
                                              train_cache_minibatch,
                                              train_weights_minibatch,
                                              train_ids_minibatch);
          // Changes each epoch, so check against original param for -1
          if(vm_["eval_every"].as<int>() == -1) eval_every_ = train_instances;
        }
        // Shuffle the access order
        std::shuffle(train_ids_minibatch.begin(), train_ids_minibatch.end(), *dynet::rndeng);
        loc = 0;
        sent_loc = 0;
        last_print = 0;
        ++epoch;
        if(epoch >= epochs_) return;
      }
      dynet::ComputationGraph cg;
      encdec.NewGraph(cg);
      // encdec.BuildSentGraph(train_src[train_ids[loc]], train_trg[train_ids[loc]], train_cache[train_ids[loc]], true, cg, train_ll);
      if(scheduled_samp_) {
        float val = (epoch_frac-scheduled_samp_)/scheduled_samp_;
        samp_prob = 1/(1+exp(val));
      }
      dynet::expr::Expression loss_exp = encdec.BuildSentGraph(
          train_src_minibatch[train_ids_minibatch[loc]],
          train_trg_minibatch[train_ids_minibatch[loc]],
          (train_cache_minibatch.size() ? train_cache_minibatch[train_ids_minibatch[loc]] : empty_cache),
          (train_weights_minibatch.size() ? &train_weights_minibatch[train_ids_minibatch[loc]] : nullptr),
          samp_prob,
          true,
          cg,
          train_ll);
      sent_loc += train_trg_minibatch[train_ids_minibatch[loc]].size();
      curr_sent_loc += train_trg_minibatch[train_ids_minibatch[loc]].size();
      epoch_frac += 1.f/train_ids_minibatch.size();
      // cg.PrintGraphviz();
      train_ll.loss_ += as_scalar(cg.incremental_forward(loss_exp));
      cg.backward(loss_exp);
      trainer->update(learning_scale);
      ++loc;
      if(sent_loc / 100 != last_print || curr_sent_loc >= eval_every_ || epochs_ == epoch) {
        last_print = sent_loc / 100;
        float elapsed = time.Elapsed();
        cerr << "Epoch " << epoch+1 << " sent " << sent_loc << ": " << train_ll.PrintStats() << ", rate=" << learning_scale*learning_rate << ", time=" << elapsed << " (" << train_ll.words_/elapsed << " w/s)" << endl;
        if(epochs_ == epoch) break;
      }
    }
    // Measure development perplexity
    if(do_dev) {
      time = Timer();
      std::vector<OutputType> empty_cache;
      encdec.SetDropout(0.f);
      for(int i : boost::irange(0, (int)dev_src_minibatch.size())) {
        dynet::ComputationGraph cg;
        encdec.NewGraph(cg);
        // encdec.BuildSentGraph(dev_src[i], dev_trg[i], empty_cache, false, cg, dev_ll);
        dynet::expr::Expression loss_exp = encdec.BuildSentGraph(dev_src_minibatch[i], dev_trg_minibatch[i], empty_cache, nullptr, 0.f, false, cg, dev_ll);
        dev_ll.loss_ += as_scalar(cg.incremental_forward(loss_exp));
      }
      float elapsed = time.Elapsed();
      cerr << "Epoch " << epoch+1 << " dev: " << dev_ll.PrintStats() << ", rate=" << learning_scale*learning_rate << ", time=" << elapsed << " (" << dev_ll.words_/elapsed << " w/s)" << endl;
    }
    // Adjust the learning rate
    trainer->update_epoch();
    // trainer->status(); cerr << endl;
    // Check the learning rate
    if(last_loss != last_loss)
      THROW_ERROR("Likelihood is not a number, dying...");
    dynet::real my_loss = do_dev ? dev_ll.loss_ : train_ll.loss_;
    if(my_loss > last_loss)
      learning_scale *= rate_decay_;
    last_loss = my_loss;
    // Open the output stream
    if(best_loss > my_loss) {
      ofstream out(model_out_file_.c_str());
      if(!out) THROW_ERROR("Could not open output file: " << model_out_file_);
      cerr << "*** Found the best model yet! Printing model to " << model_out_file_ << endl;
      // Write the model (TODO: move this to a separate file?)
      WriteDict(vocab_src, out);
      WriteDict(vocab_trg, out);
      encdec.Write(out);
      ModelUtils::WriteModelText(out, model);
      best_loss = my_loss;
      evals_since_improvement = 0;
    } else {
      ++evals_since_improvement;
      if(early_stop != -1 && evals_since_improvement == early_stop) {
        cerr << "No improvement in " << evals_since_improvement << " evals, stopping early" << endl;
        break;
      }
    }
    // If the rate is less than the threshold
    if(learning_scale * learning_rate < rate_thresh_)
      break;
  }
}

inline dynet::expr::Expression CalcRisk(const Sentence & ref,
                                      const vector<Sentence> & trg_samples,
                                      dynet::expr::Expression trg_log_probs,
                                      const EvalMeasure & eval,
                                      float scaling,
                                      bool dedup,
                                      dynet::ComputationGraph & cg) {
    // If scaling the distribution do it
    if(scaling != 1.f)
        trg_log_probs = trg_log_probs * scaling;
    vector<float> trg_log_probs_vec = as_vector(trg_log_probs.value());
    vector<float> eval_scores(trg_samples.size(), 0.f);
    set<Sentence> sent_dup;
    vector<float> mask(trg_samples.size(), 0.f);
    for(size_t i = 0; i < trg_samples.size(); i++) {
        auto it = sent_dup.find(trg_samples[i]);
        if(it != sent_dup.end()) { 
            mask[i] = FLT_MAX;
        } else {
            eval_scores[i] = eval.CalculateStats(ref, trg_samples[i])->ConvertToScore();
            sent_dup.insert(trg_samples[i]);
        }
        // cerr << "i=" << i << ", tlp=" << trg_log_probs_vec[i] << ", eval=" << eval_scores[i] << ", len=" << trg_samples[i].size() << endl;
    }
    // cerr << "---------------------" << endl;
    if(sent_dup.size() != trg_samples.size())
        trg_log_probs = trg_log_probs + input(cg, dynet::Dim({(unsigned int)trg_samples.size()}), mask);
    // Calculate expected and return loss
    return -input(cg, dynet::Dim({1, (unsigned int)trg_samples.size()}), eval_scores) * softmax(trg_log_probs);
}

// Performs minimimum risk training according to the following paper:
//  Minimum Risk Training for Neural Machine Translation
//  Shen et al. (http://arxiv.org/abs/1512.02433)
template<class ModelType>
void LamtramTrain::MinRiskTraining(const vector<Sentence> & train_src,
                                   const vector<Sentence> & train_trg,
                                   const vector<int> & train_fold_ids,
                                   const vector<Sentence> & dev_src,
                                   const vector<Sentence> & dev_trg,
                                   const dynet::Dict & vocab_src,
                                   const dynet::Dict & vocab_trg,
                                   const EvalMeasure & eval,
                                   dynet::Model & model,
                                   ModelType & encdec) {

  // Sanity checks
  assert(train_src.size() == train_trg.size());
  assert(dev_src.size() == dev_trg.size());

  TrainerPtr trainer = GetTrainer(vm_["trainer"].as<string>(), vm_["learning_rate"].as<float>(), model);
  int max_len = vm_["minrisk_max_len"].as<int>();
  int num_samples = vm_["minrisk_num_samples"].as<int>();
  float scaling = vm_["minrisk_scaling"].as<float>();
  bool include_ref = vm_["minrisk_include_ref"].as<bool>();
  bool dedup = vm_["minrisk_dedup"].as<bool>();

  // Find the span of the folds
  vector<pair<int,int> > fold_id_spans;
  for(size_t i = 0; i < train_fold_ids.size(); i++) {
    if(train_fold_ids[i] >= fold_id_spans.size()) {
      fold_id_spans.resize(train_fold_ids[i]+1, make_pair(i,i+1));
    } else {
      fold_id_spans[train_fold_ids[i]].second = i+1;
    }
  }
  
  // Learning rate
  dynet::real learning_rate = vm_["learning_rate"].as<float>();
  dynet::real learning_scale = 1.0;

  // Create a sentence list and random generator
  std::vector<int> train_ids(train_src.size());
  std::iota(train_ids.begin(), train_ids.end(), 0);
  // Perform the training
  std::vector<dynet::expr::Expression> empty_hist;
  dynet::real last_loss = 1e99, best_loss = 1e99;
  bool do_dev = dev_src.size() != 0;
  int loc = train_ids.size(), epoch = -1, sent_loc = 0, last_print = 0;
  float epoch_frac = 0.f;
  while(true) {
    // Start the training
    LossStats train_loss, dev_loss;
    Timer time;
    encdec.SetDropout(dropout_);
    for(int curr_sent_loc = 0; curr_sent_loc < eval_every_; ) {
      if(loc == (int)train_ids.size()) {
        // Shuffle the access order
        for(const pair<int,int> & fold_span : fold_id_spans)
          std::shuffle(train_ids.begin()+fold_span.first, train_ids.begin()+fold_span.second, *dynet::rndeng);
        loc = 0;
        last_print = 0;
        sent_loc = 0;
        ++epoch;
        if(epoch >= epochs_) return;
      }
      // Create the graph
      dynet::ComputationGraph cg;
      encdec.GetDecoderPtr()->GetSoftmax().UpdateFold(train_fold_ids[train_ids[loc]]+1);
      encdec.NewGraph(cg);
      // Sample sentences
      std::vector<Sentence> trg_samples;
      dynet::expr::Expression trg_log_probs = encdec.SampleTrgSentences(train_src[train_ids[loc]], 
                                                                      (include_ref ? &train_trg[train_ids[loc]] : NULL),
                                                                      num_samples, max_len, true, cg, trg_samples);
      dynet::expr::Expression trg_loss = CalcRisk(train_trg[train_ids[loc]], trg_samples, trg_log_probs, eval, scaling, dedup, cg);
      // Increment
      sent_loc++; curr_sent_loc++;
      epoch_frac += 1.f/train_src.size(); 
      train_loss.loss_ += as_scalar(cg.incremental_forward(trg_loss));
      train_loss.sents_++;
      // cg.PrintGraphviz();
      cg.backward(trg_loss);
      trainer->update(learning_scale);
      ++loc;
      if(sent_loc / 100 != last_print || curr_sent_loc >= eval_every_ || epochs_ == epoch) {
        last_print = sent_loc / 100;
        float elapsed = time.Elapsed();
        cerr << "Epoch " << epoch+1 << " sent " << sent_loc << ": score=" << -train_loss.CalcSentLoss() << ", rate=" << learning_scale*learning_rate << ", time=" << elapsed << " (" << train_loss.sents_/elapsed << " sent/s)" << endl;
        if(epochs_ == epoch) break;
      }
    }
    // Measure development perplexity
    if(do_dev) {
      time = Timer();
      encdec.SetDropout(0.f);
      for(int i : boost::irange(0, (int)dev_src.size())) {
          dynet::ComputationGraph cg;
          encdec.NewGraph(cg);
          // Sample sentences
          std::vector<Sentence> trg_samples;
          Expression trg_log_probs = encdec.SampleTrgSentences(dev_src[i], 
                                                               (include_ref ? &dev_trg[i] : NULL),
                                                               num_samples, max_len, true, cg, trg_samples);
          dynet::expr::Expression loss_exp = CalcRisk(dev_trg[i], trg_samples, trg_log_probs, eval, scaling, dedup, cg);
          dev_loss.loss_ += as_scalar(cg.incremental_forward(loss_exp));
          dev_loss.sents_++;
      }
      float elapsed = time.Elapsed();
      cerr << "Epoch " << epoch+1 << " dev: score=" << -dev_loss.CalcSentLoss() << ", rate=" << learning_scale*learning_rate << ", time=" << elapsed << " (" << dev_loss.sents_/elapsed << " sent/s)" << endl;
    }
    // Adjust the learning rate
    trainer->update_epoch();
    // trainer->status(); cerr << endl;
    // Check the learning rate
    if(last_loss != last_loss)
      THROW_ERROR("Loss is not a number, dying...");
    dynet::real my_loss = do_dev ? dev_loss.loss_ : train_loss.loss_;
    if(my_loss > last_loss)
      learning_scale *= rate_decay_;
    last_loss = my_loss;
    // Open the output stream
    if(best_loss > my_loss) {
      ofstream out(model_out_file_.c_str());
      if(!out) THROW_ERROR("Could not open output file: " << model_out_file_);
      cerr << "*** Found the best model yet! Printing model to " << model_out_file_ << endl;
      // Write the model (TODO: move this to a separate file?)
      WriteDict(vocab_src, out);
      WriteDict(vocab_trg, out);
      encdec.Write(out);
      ModelUtils::WriteModelText(out, model);
      best_loss = my_loss;
    }
    // If the rate is less than the threshold
    if(learning_scale * learning_rate < rate_thresh_)
      break;
  }
}

void LamtramTrain::LoadFile(const std::string filename, bool add_last, dynet::Dict & vocab, std::vector<Sentence> & sents) {
  ifstream iftrain(filename.c_str());
  if(!iftrain) THROW_ERROR("Could not find training file: " << filename);
  string line;
  int line_no = 0;
  while(getline(iftrain, line)) {
    line_no++;
    Sentence sent = ParseWords(vocab, line, add_last);
    if(sent.size() == (add_last ? 1 : 0))
      THROW_ERROR("Empty line found in " << filename << " at " << line_no << endl);
    sents.push_back(sent);
  }
  iftrain.close();
}

void LamtramTrain::LoadLabels(const std::string filename, dynet::Dict & vocab, std::vector<int> & labs) {
  ifstream iftrain(filename.c_str());
  if(!iftrain) THROW_ERROR("Could not find training file: " << filename);
  string line;
  while(getline(iftrain, line))
    labs.push_back(vocab.convert(line));
  iftrain.close();
}

void LamtramTrain::LoadWeights(const std::string filename, std::vector<float> & weights) {
  ifstream ifweights(filename.c_str());
  if(!ifweights) THROW_ERROR("Could not find weights file: " << filename);
  string line;
  while(getline(ifweights, line))
    weights.push_back(boost::lexical_cast<float>(line));
  ifweights.close();
}

LamtramTrain::TrainerPtr LamtramTrain::GetTrainer(const std::string & trainer_id, const dynet::real learning_rate, dynet::Model & model) {
  TrainerPtr trainer;
  if(trainer_id == "sgd") {
    trainer.reset(new dynet::SimpleSGDTrainer(&model, learning_rate));
  } else if(trainer_id == "momentum") {
    trainer.reset(new dynet::MomentumSGDTrainer(&model, learning_rate));
  } else if(trainer_id == "adagrad") {
    trainer.reset(new dynet::AdagradTrainer(&model, learning_rate));
  } else if(trainer_id == "adadelta") {
    trainer.reset(new dynet::AdadeltaTrainer(&model, learning_rate));
  } else if(trainer_id == "adam") {
    trainer.reset(new dynet::AdamTrainer(&model, learning_rate));
  } else {
    THROW_ERROR("Illegal trainer variety: " << trainer_id);
  }
  trainer->sparse_updates_enabled = false;
  return trainer;
}
