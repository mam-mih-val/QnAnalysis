#include "CorrectionTask.h"

#include <iostream>
#include <memory>

#include "AnalysisTree/DataHeader.hpp"
#include "AnalysisTree/TreeReader.hpp"
#include "QVector.h"

#include <config/Config.h>

namespace Qn {

using std::string;
using std::vector;

void CorrectionTask::InitVariables() {
  // Add all needed variables
  short ivar{0}, ibranch{0};

  for (auto& entry : var_manager_->VarEntries()) {
    assert(entry.GetNumberOfBranches() == 1);

    if (entry.GetBranches()[0]->GetType() == AnalysisTree::DetType::kModule) {
      ibranch++;
      continue;
    }// Should be handled separately

    for (auto& var : entry.Variables()) {
      if (var.GetName() != "_Ones") {
        var.SetId(ivar);
        manager_.AddVariable(var.GetName(), var.GetId(), var.GetSize());
        //        var.Print();
        ivar += var.GetSize();
      }
    }

    auto type = entry.GetBranches()[0]->GetType();
    if (type != AnalysisTree::DetType::kEventHeader && type != AnalysisTree::DetType::kModule) {
      manager_.AddVariable(entry.GetBranches()[0]->GetName() + "_Filled", ivar, 1);
      is_filled_.insert(std::make_pair(ibranch, ivar));
      ivar++;
    }

    ibranch++;
  }

  for (auto& qvec : global_config_->ChannelConfig()) {
    auto& phi = qvec.PhiVar();
    phi.SetId(ivar);
    manager_.AddVariable(qvec.GetName() + "_" + phi.GetName(), phi.GetId(), phi.GetSize());
    ivar += phi.GetSize();
    auto& weight = qvec.WeightVar();
    weight.SetId(ivar);
    manager_.AddVariable(qvec.GetName() + "_" + weight.GetName(), weight.GetId(), weight.GetSize());
    //    weight.Print();
    //    phi.Print();
    ivar += weight.GetSize();
  }

  for (const auto& event_var : global_config_->GetEventVars()) {
    manager_.AddEventVariable(event_var.GetName());
  }
}

void CorrectionTask::Init(std::map<std::string, void*>&) {
  out_file_ = static_cast<std::shared_ptr<TFile>>(TFile::Open("correction_out.root", "recreate"));
  out_file_->cd();
  out_tree_ = new TTree("tree", "tree");
  manager_.SetCalibrationInputFileName(in_calibration_file_name_);
  manager_.SetFillOutputTree(true);
  manager_.SetFillCalibrationQA(true);
  manager_.SetFillValidationQA(true);
  manager_.ConnectOutputTree(out_tree_);

  InitVariables();

  for (const auto& axis : global_config_->GetCorrectionAxes()) {
    manager_.AddCorrectionAxis(axis);
  }

  for (const auto& qvec_ptr : global_config_->q_vectors) {
    if (qvec_ptr->GetType() == Flow::Base::EQVectorType::TRACK) {
      auto track_qv = std::dynamic_pointer_cast<Flow::Base::QVectorTrack>(qvec_ptr);
      const string& name = track_qv->GetName();
      auto qn_weight = track_qv->GetWeightVar().GetName() == "_Ones" ? "Ones" : track_qv->GetWeightVar().GetName();
      manager_.AddDetector(name, DetectorType::TRACK, track_qv->GetPhiVar().GetName(), qn_weight, track_qv->GetAxes(), {1, 2});
      Info(__func__, "Add track detector '%s'", name.c_str());
      SetCorrectionSteps(track_qv.operator*());
      for (const auto& cut : track_qv->GetCuts()) {//NOTE cannot apply cuts on more than 1 variable
        manager_.AddCutOnDetector(name, {cut.GetVariable().GetName().c_str()}, cut.GetFunction(), cut.GetDescription());
      }
    } else if (qvec_ptr->GetType() == Flow::Base::EQVectorType::CHANNEL) {
      auto channel_qv = std::dynamic_pointer_cast<Flow::Base::QVectorChannel>(qvec_ptr);
      const string name = channel_qv->GetName();
      auto qn_phi = name + "_" + channel_qv->GetPhiVar().GetName();
      auto qn_weight = channel_qv->GetWeightVar().GetName() == "_Ones" ? "Ones" : name + "_" + channel_qv->GetWeightVar().GetName();
      manager_.AddDetector(name, DetectorType::CHANNEL, qn_phi, qn_weight, {/* no axes to be passed */}, {1});
      Info(__func__, "Add channel detector '%s'", name.c_str());
      SetCorrectionSteps(channel_qv.operator*());
    } else if (qvec_ptr->GetType() == Flow::Base::EQVectorType::EVENT_PSI) {
      const string name = qvec_ptr->GetName();
      auto qn_weight = qvec_ptr->GetWeightVar().GetName() == "_Ones" ? "Ones" : qvec_ptr->GetWeightVar().GetName();
      manager_.AddDetector(qvec_ptr->GetName(), DetectorType::CHANNEL, qvec_ptr->GetPhiVar().GetName(), qn_weight, {}, {1, 2});
      Info(__func__, "Add event PSI '%s'", name.c_str());
    }
  }

  // Add Psi_RP
  if (global_config_->IsSimulation()) {
    const auto& qvec = global_config_->GetPsiQvector();
    manager_.AddDetector(qvec.GetName(), DetectorType::CHANNEL, qvec.GetPhiVar().GetName(), qvec.GetWeightVar().GetName(), {}, {1, 2});
    SetCorrectionSteps(qvec);
  }

  AddQAHisto();

  //Initialization of framework
  manager_.InitializeOnNode();
  manager_.SetCurrentRunName("test");
}

/**
* Main method. Executed every event
*/
void CorrectionTask::Exec() {
  manager_.Reset();
  double* container = manager_.GetVariableContainer();

  for (const auto& entry : var_manager_->GetVarEntries()) {
    if (entry.GetBranches()[0]->GetType() == AnalysisTree::DetType::kEventHeader) {
      short ivar{0};
      for (const auto& var : entry.GetVariables()) {
        container[var.GetId()] = entry.GetValues().at(0)[ivar];
        ivar++;
      }
    }
  }
  for (const auto& qvec : global_config_->GetChannelConfig()) {
    const auto& phi = qvec.GetPhiVar();
    const auto& weight = qvec.GetWeightVar();
    const auto& branch = var_manager_->GetVarEntries().at(qvec.GetVarEntryId());
    int i_channel{0};
    for (int i : qvec.GetModuleIds()) {
      container[phi.GetId() + i_channel] = data_header_->GetModulePhi(0, i);//TODO fix hardcoded 0
      if (i < branch.GetValues().size()) {
        container[weight.GetId() + i_channel] = branch.GetValues()[i].at(0);//TODO fix hardcoded 0
      } else {
        container[weight.GetId() + i_channel] = 0.;
      }
      i_channel++;
    }
  }
  manager_.ProcessEvent();
  manager_.FillChannelDetectors();

  FillTracksQvectors();

  manager_.ProcessCorrections();
}
/**
* Fill the information from Tracks, Particles and Hits. We assume that Tracking Q-vectors are not constructed from
* Modules. Information from EventHeaders and Modules should be filled before.
*/
void CorrectionTask::FillTracksQvectors() {
  double* container = manager_.GetVariableContainer();
  short ibranch{0};
  for (const auto& entry : var_manager_->GetVarEntries()) {
    assert(entry.GetNumberOfBranches() == 1);
    auto type = entry.GetBranches()[0]->GetType();
    if (type == AnalysisTree::DetType::kEventHeader || type == AnalysisTree::DetType::kModule) {
      ibranch++;
      continue;// skip EventHeader and ModuleDetectors
    }
    const int n_channels = entry.GetValues().size();
    for (int i = 0; i < n_channels; ++i) {
      for (auto const& fill : is_filled_) {
        container[fill.second] = fill.first == ibranch ? 1. : -1.;// 1 for current, -1 for all others
      }
      short ivar{0};
      for (const auto& var : entry.GetVariables()) {
        container[var.GetId()] = entry.GetValues()[i].at(ivar);
        ivar++;
      }
      manager_.FillTrackingDetectors();
    }
    ibranch++;
  }
}

/**
* Adding QA histograms to CorrectionManager
*/

void CorrectionTask::AddQAHisto() {
  for (const auto& qvec : global_config_->GetQvectorsConfig()) {
    for (const auto& qa : qvec.GetQAHistograms()) {
      if (qa.axes.size() == 1) {
        manager_.AddHisto1D(qvec.GetName(), qa.axes.at(0).GetQnAxis());
      } else if (qa.axes.size() == 2) {
        manager_.AddHisto2D(qvec.GetName(), {qa.axes.at(0).GetQnAxis(), qa.axes.at(1).GetQnAxis()});
      } else {
        throw std::runtime_error("QA histograms with more than 2 axis (or less than one) are not supported.");
      }
    }
  }

  for (const auto& qvec : global_config_->GetChannelConfig()) {
    for (const auto& qa : qvec.GetQAHistograms()) {
      if (qa.axes.size() == 1) {
        auto axis = qa.axes.at(0).GetQnAxis();
        axis.SetName(qvec.GetName() + "_" + axis.Name());
        manager_.AddHisto1D(qvec.GetName(), axis);
      } else {
        throw std::runtime_error("FIX ME");
      }
    }
  }

  //  for(const auto& histo : qa_histos_){
  //    const auto& [name, axis] = histo;
  //    if( axis.size() == 1 )
  //      manager_.AddHisto1D(name, axis.at(0));
  //    if( axis.size() == 2 )
  //      manager_.AddHisto2D(name, axis);
  //  }
}

void CorrectionTask::Finish() {
  manager_.Finalize();

  out_file_->cd();
  out_tree_->Write("tree");

  auto* correction_list = manager_.GetCorrectionList();
  auto* correction_qa_list = manager_.GetCorrectionQAList();
  correction_list->Write("CorrectionHistograms", TObject::kSingleKey);
  correction_qa_list->Write("CorrectionQAHistograms", TObject::kSingleKey);
  //  global_config_->Write("Config");

  out_file_->Close();
}

/**
* Fill list of variables needed to construct all Q-vectors and event information.
* This vector is passed to VarManager later
*/

/**
* Set correction steps in a CorrectionManager for a given Q-vector
*/
void CorrectionTask::SetCorrectionSteps(const Flow::Base::QVector& qvec) {
  std::vector<Qn::QVector::CorrectionStep> correction_steps = {Qn::QVector::CorrectionStep::PLAIN};
  const std::string& name = qvec.GetName();

  for (const auto& correction : qvec.GetCorrections()) {
    if (correction->IsA() == Qn::Recentering::Class()) {
      auto recentering = std::dynamic_pointer_cast<Qn::Recentering>(correction);
      correction_steps.emplace_back(Qn::QVector::CorrectionStep::RECENTERED);
      manager_.AddCorrectionOnQnVector(name, recentering.operator*());
      Info(__func__, "Add Recentering to '%s'", name.c_str());
    } else if (correction->IsA() == Qn::TwistAndRescale::Class()) {
      auto twist_and_rescale = std::dynamic_pointer_cast<Qn::TwistAndRescale>(correction);
      correction_steps.emplace_back(Qn::QVector::CorrectionStep::TWIST);
      correction_steps.emplace_back(Qn::QVector::CorrectionStep::RESCALED);
      manager_.AddCorrectionOnQnVector(name, twist_and_rescale.operator*());
      Info(__func__, "Add Twist-And-Rescale to '%s'", name.c_str());
    }
  }

  manager_.SetOutputQVectors(name, correction_steps);
}

}// namespace Qn
