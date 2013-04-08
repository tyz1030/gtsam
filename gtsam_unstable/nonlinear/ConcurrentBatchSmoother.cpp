/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation, 
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    ConcurrentBatchSmoother.cpp
 * @brief   A Levenberg-Marquardt Batch Smoother that implements the
 *          Concurrent Filtering and Smoothing interface.
 * @author  Stephen Williams
 */

#include <gtsam_unstable/nonlinear/ConcurrentBatchSmoother.h>
#include <gtsam_unstable/nonlinear/LinearizedFactor.h>
#include <gtsam/inference/JunctionTree.h>
#include <gtsam/base/timing.h>
#include <boost/lambda/lambda.hpp>

namespace gtsam {

/* ************************************************************************* */
void ConcurrentBatchSmoother::SymbolicPrintTree(const Clique& clique, const Ordering& ordering, const std::string indent) {
  std::cout << indent << "P( ";
  BOOST_FOREACH(Index index, clique->conditional()->frontals()){
    std::cout << DefaultKeyFormatter(ordering.key(index)) << " ";
  }
  if(clique->conditional()->nrParents() > 0) {
    std::cout << "| ";
  }
  BOOST_FOREACH(Index index, clique->conditional()->parents()){
    std::cout << DefaultKeyFormatter(ordering.key(index)) << " ";
  }
  std::cout << ")" << std::endl;

  BOOST_FOREACH(const Clique& child, clique->children()) {
    SymbolicPrintTree(child, ordering, indent+"  ");
  }
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::print(const std::string& s,
    const KeyFormatter& keyFormatter) const {
  std::cout << s;
  graph_.print("Factors:\n");
  theta_.print("Values:\n");
}

/* ************************************************************************* */
ConcurrentBatchSmoother::Result ConcurrentBatchSmoother::update(const NonlinearFactorGraph& newFactors, const Values& newTheta) {

  gttic(update);

  // Create result structure
  Result result;

  gttic(augment_system);
  // Add the new factors to the graph
  BOOST_FOREACH(const NonlinearFactor::shared_ptr& factor, newFactors) {
    insertFactor(factor);
  }
  // Add the new variables to theta
  theta_.insert(newTheta);
  gttoc(augment_system);

  // Optimize the graph, updating theta
  gttic(optimize);
  if(graph_.size() > 0){
    // Create an L-M optimizer
    Values linpoint;
    linpoint.insert(theta_);
    if(rootValues_.size() > 0) {
      linpoint.insert(rootValues_);
    }
    LevenbergMarquardtOptimizer optimizer(graph_, linpoint, parameters_);

    // Use a custom optimization loop so the linearization points can be controlled
    double currentError;
    do {
      // Do next iteration
      gttic(optimizer_iteration);
      currentError = optimizer.error();
      optimizer.iterate();
      gttoc(optimizer_iteration);

      // Force variables associated with root keys to keep the same linearization point
      gttic(enforce_consistency);
      if(rootValues_.size() > 0) {
        // Put the old values of the root keys back into the optimizer state
        optimizer.state().values.update(rootValues_);
        optimizer.state().error = graph_.error(optimizer.state().values);
      }
      gttoc(enforce_consistency);

      // Maybe show output
      if(parameters_.verbosity >= NonlinearOptimizerParams::VALUES) optimizer.values().print("newValues");
      if(parameters_.verbosity >= NonlinearOptimizerParams::ERROR) std::cout << "newError: " << optimizer.error() << std::endl;
    } while(optimizer.iterations() < parameters_.maxIterations &&
        !checkConvergence(parameters_.relativeErrorTol, parameters_.absoluteErrorTol,
            parameters_.errorTol, currentError, optimizer.error(), parameters_.verbosity));

    // Update theta from the optimizer, then remove root variables
    theta_ = optimizer.values();
    BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, rootValues_) {
      theta_.erase(key_value.key);
    }

    result.iterations = optimizer.state().iterations;
    result.nonlinearVariables = theta_.size();
    result.linearVariables = rootValues_.size();
    result.error = optimizer.state().error;
  }
  gttoc(optimize);

  // Move all of the Pre-Sync code to the end of the update. This allows the smoother to perform these
  // calculations while the filter is still running
  gttic(presync);
  // Calculate and store the information passed up to the root clique. This requires:
  //   1) Calculate an ordering that forces the rootKey variables to be in the root
  //   2) Perform an elimination, constructing a Bayes Tree from the currnet
  //      variable values. This elimination will use the iSAM2 version of a clique so
  //      that cached factors are stored
  //   3) Verify the root's cached factors involve only root keys; all others should
  //      be marginalized
  //   4) Convert cached factors into 'Linearized' nonlinear factors

  if(rootValues_.size() > 0) {
    // Force variables associated with root keys to keep the same linearization point
    gttic(enforce_consistency);
    Values linpoint;
    linpoint.insert(theta_);
    linpoint.insert(rootValues_);

//linpoint.print("ConcurrentBatchSmoother::presync()  LinPoint:\n");

    gttoc(enforce_consistency);

    // Calculate a root-constrained ordering
    gttic(compute_ordering);
    std::map<Key, int> constraints;
    BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, rootValues_) {
      constraints[key_value.key] = 1;
    }
    Ordering ordering = *graph_.orderingCOLAMDConstrained(linpoint, constraints);
    gttoc(compute_ordering);

    // Create a Bayes Tree using iSAM2 cliques
    gttic(create_bayes_tree);
    JunctionTree<GaussianFactorGraph, ISAM2Clique> jt(*graph_.linearize(linpoint, ordering));
    ISAM2Clique::shared_ptr root = jt.eliminate(parameters_.getEliminationFunction());
    BayesTree<GaussianConditional, ISAM2Clique> bayesTree;
    bayesTree.insert(root);
    gttoc(create_bayes_tree);

//ordering.print("ConcurrentBatchSmoother::presync()  Ordering:\n");
std::cout << "ConcurrentBatchSmoother::presync()  Root Keys: "; BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, rootValues_) { std::cout << DefaultKeyFormatter(key_value.key) << " "; } std::cout << std::endl;
std::cout << "ConcurrentBatchSmoother::presync()  Bayes Tree:" << std::endl;
SymbolicPrintTree(root, ordering, "  ");

    // Extract the marginal factors from the smoother
    // For any non-filter factor that involves a root variable,
    // calculate its marginal on the root variables using the
    // current linearization point.

    // Find all of the smoother branches as the children of root cliques that are not also root cliques
    gttic(find_smoother_branches);
    std::set<ISAM2Clique::shared_ptr> rootCliques;
    std::set<ISAM2Clique::shared_ptr> smootherBranches;
    BOOST_FOREACH(const Values::ConstKeyValuePair& key_value, rootValues_) {
      const ISAM2Clique::shared_ptr& clique = bayesTree.nodes().at(ordering.at(key_value.key));
      if(clique) {
        rootCliques.insert(clique);
        smootherBranches.insert(clique->children().begin(), clique->children().end());
      }
    }
    BOOST_FOREACH(const ISAM2Clique::shared_ptr& rootClique, rootCliques) {
      smootherBranches.erase(rootClique);
    }
    gttoc(find_smoother_branches);

    // Extract the cached factors on the root cliques from the smoother branches
    gttic(extract_cached_factors);
    GaussianFactorGraph cachedFactors;
    BOOST_FOREACH(const ISAM2Clique::shared_ptr& clique, smootherBranches) {
      cachedFactors.push_back(clique->cachedFactor());
    }
    gttoc(extract_cached_factors);

std::cout << "ConcurrentBatchSmoother::presync()  Cached Factors Before:" << std::endl;
BOOST_FOREACH(const GaussianFactor::shared_ptr& factor, cachedFactors) {
  std::cout << "  g( ";
  BOOST_FOREACH(Index index, factor->keys()) {
    std::cout << DefaultKeyFormatter(ordering.key(index)) << " ";
  }
  std::cout << ")" << std::endl;
}

    // Marginalize out any additional (non-root) variables
    gttic(marginalize_extra_variables);
    // The rootKeys have been ordered last, so their linear indices will be { linpoint.size()-rootKeys.size() :: linpoint.size()-1 }
    Index minRootIndex = linpoint.size() - rootValues_.size();
    // Calculate the set of keys to be marginalized
    FastSet<Index> cachedIndices = cachedFactors.keys();
    std::vector<Index> marginalizeIndices;
    std::remove_copy_if(cachedIndices.begin(), cachedIndices.end(), std::back_inserter(marginalizeIndices), boost::lambda::_1 >= minRootIndex);

std::cout << "ConcurrentBatchSmoother::presync()  Marginalize Keys: ";
BOOST_FOREACH(Index index, marginalizeIndices) { std::cout << DefaultKeyFormatter(ordering.key(index)) << " "; }
std::cout << std::endl;

    // If non-root-keys are present, marginalize them out
    if(marginalizeIndices.size() > 0) {
      // Eliminate the extra variables, stored the remaining factors back into the 'cachedFactors' graph
      GaussianConditional::shared_ptr conditional;
      boost::tie(conditional, cachedFactors) = cachedFactors.eliminate(marginalizeIndices, parameters_.getEliminationFunction());
    }
    gttoc(marginalize_extra_variables);

std::cout << "ConcurrentBatchSmoother::presync()  Cached Factors After:" << std::endl;
BOOST_FOREACH(const GaussianFactor::shared_ptr& factor, cachedFactors) {
  std::cout << "  g( ";
  BOOST_FOREACH(Index index, factor->keys()) {
    std::cout << DefaultKeyFormatter(ordering.key(index)) << " ";
  }
  std::cout << ")" << std::endl;
}

    // Convert factors into 'Linearized' nonlinear factors
    gttic(store_cached_factors);
    smootherSummarization_.resize(0);
    BOOST_FOREACH(const GaussianFactor::shared_ptr& gaussianFactor, cachedFactors) {
      LinearizedGaussianFactor::shared_ptr factor;
      if(const JacobianFactor::shared_ptr rhs = boost::dynamic_pointer_cast<JacobianFactor>(gaussianFactor))
        factor = LinearizedJacobianFactor::shared_ptr(new LinearizedJacobianFactor(rhs, ordering, linpoint));
      else if(const HessianFactor::shared_ptr rhs = boost::dynamic_pointer_cast<HessianFactor>(gaussianFactor))
        factor = LinearizedHessianFactor::shared_ptr(new LinearizedHessianFactor(rhs, ordering, linpoint));
      else
        throw std::invalid_argument("In ConcurrentBatchSmoother::presync(...), cached factor is neither a JacobianFactor nor a HessianFactor");
      smootherSummarization_.push_back(factor);
    }
    gttoc(store_cached_factors);

std::cout << "ConcurrentBatchSmoother::presync()  Smoother Summarization:" << std::endl;
BOOST_FOREACH(const NonlinearFactor::shared_ptr& factor, smootherSummarization_) {
  std::cout << "  f( ";
  BOOST_FOREACH(Key key, factor->keys()) {
    std::cout << DefaultKeyFormatter(key) << " ";
  }
  std::cout << ")" << std::endl;
}

  }
  gttoc(presync);

  gttoc(update);

  return result;
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::presync() {

  gttic(presync);


  gttoc(presync);
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::getSummarizedFactors(NonlinearFactorGraph& summarizedFactors) {

  gttic(get_summarized_factors);

  // Copy the previous calculated smoother summarization factors into the output
  summarizedFactors.push_back(smootherSummarization_);

  gttoc(get_summarized_factors);
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::synchronize(const NonlinearFactorGraph& smootherFactors, const Values& smootherValues,
    const NonlinearFactorGraph& summarizedFactors, const Values& rootValues) {

  gttic(synchronize);

  // Remove the previous filter summarization from the graph
  BOOST_FOREACH(size_t slot, filterSummarizationSlots_) {
    removeFactor(slot);
  }
  filterSummarizationSlots_.clear();

  // Insert the new filter summarized factors
  BOOST_FOREACH(const NonlinearFactor::shared_ptr& factor, summarizedFactors) {
    filterSummarizationSlots_.push_back(insertFactor(factor));
  }

  // Insert the new smoother factors
  BOOST_FOREACH(const NonlinearFactor::shared_ptr& factor, smootherFactors) {
    insertFactor(factor);
  }

  // Insert new linpoints into the values
  theta_.insert(smootherValues);

  // Update the list of root keys
  rootValues_ = rootValues;

  gttoc(synchronize);
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::postsync() {

  gttic(postsync);

  gttoc(postsync);
}

/* ************************************************************************* */
size_t ConcurrentBatchSmoother::insertFactor(const NonlinearFactor::shared_ptr& factor) {

  gttic(insert_factors);

  // Insert the factor into an existing hole in the factor graph, if possible
  size_t slot;
  if(availableSlots_.size() > 0) {
    slot = availableSlots_.front();
    availableSlots_.pop();
    graph_.replace(slot, factor);
  } else {
    slot = graph_.size();
    graph_.push_back(factor);
  }
  // Update the FactorIndex
  BOOST_FOREACH(Key key, *factor) {
    factorIndex_[key].insert(slot);
  }

  gttoc(insert_factors);

  return slot;
}

/* ************************************************************************* */
void ConcurrentBatchSmoother::removeFactor(size_t slot) {

  gttic(remove_factors);

  // Remove references to this factor from the FactorIndex
  BOOST_FOREACH(Key key, *(graph_.at(slot))) {
    factorIndex_[key].erase(slot);
  }
  // Remove this factor from the graph
  graph_.remove(slot);
  // Mark the factor slot as avaiable
  availableSlots_.push(slot);

  gttoc(remove_factors);
}

/* ************************************************************************* */
std::set<size_t> ConcurrentBatchSmoother::findFactorsWithAny(const std::set<Key>& keys) const {
  // Find the set of factor slots for each specified key
  std::set<size_t> factorSlots;
  BOOST_FOREACH(Key key, keys) {
    FactorIndex::const_iterator iter = factorIndex_.find(key);
    if(iter != factorIndex_.end()) {
      factorSlots.insert(iter->second.begin(), iter->second.end());
    }
  }

  return factorSlots;
}

/* ************************************************************************* */
std::set<size_t> ConcurrentBatchSmoother::findFactorsWithOnly(const std::set<Key>& keys) const {
  // Find the set of factor slots with any of the provided keys
  std::set<size_t> factorSlots = findFactorsWithAny(keys);
  // Test each factor for non-specified keys
  std::set<size_t>::iterator slot = factorSlots.begin();
  while(slot != factorSlots.end()) {
    const NonlinearFactor::shared_ptr& factor = graph_.at(*slot);
    std::set<Key> factorKeys(factor->begin(), factor->end()); // ensure the keys are sorted
    if(!std::includes(keys.begin(), keys.end(), factorKeys.begin(), factorKeys.end())) {
      factorSlots.erase(slot++);
    } else {
      ++slot;
    }
  }

  return factorSlots;
}

/* ************************************************************************* */
NonlinearFactor::shared_ptr ConcurrentBatchSmoother::marginalizeKeysFromFactor(const NonlinearFactor::shared_ptr& factor, const std::set<Key>& keysToKeep, const Values& theta) const {

factor->print("Factor Before:\n");

  // Sort the keys for this factor
  std::set<Key> factorKeys;
  BOOST_FOREACH(Key key, *factor) {
    factorKeys.insert(key);
  }

  // Calculate the set of keys to marginalize
  std::set<Key> marginalizeKeys;
  std::set_difference(factorKeys.begin(), factorKeys.end(), keysToKeep.begin(), keysToKeep.end(), std::inserter(marginalizeKeys, marginalizeKeys.end()));
  std::set<Key> remainingKeys;
  std::set_intersection(factorKeys.begin(), factorKeys.end(), keysToKeep.begin(), keysToKeep.end(), std::inserter(remainingKeys, remainingKeys.end()));

  //
  if(marginalizeKeys.size() == 0) {
    // No keys need to be marginalized out. Simply return the original factor.
    return factor;
  } else if(marginalizeKeys.size() == factor->size()) {
    // All keys need to be marginalized out. Return an empty factor
    return NonlinearFactor::shared_ptr();
  } else {
    // (0) Create an ordering with the remaining keys last
    Ordering ordering;
    BOOST_FOREACH(Key key, marginalizeKeys) {
      ordering.push_back(key);
    }
    BOOST_FOREACH(Key key, remainingKeys) {
      ordering.push_back(key);
    }
ordering.print("Ordering:\n");

    //  (1) construct a linear factor graph
    GaussianFactorGraph graph;
    graph.push_back( factor->linearize(theta, ordering) );
graph.at(0)->print("Linear Factor Before:\n");

    //  (2) solve for the marginal factor
    // Perform partial elimination, resulting in a conditional probability ( P(MarginalizedVariable | RemainingVariables)
    // and factors on the remaining variables ( f(RemainingVariables) ). These are the factors we need to add to iSAM2
    std::vector<Index> variables;
    BOOST_FOREACH(Key key, marginalizeKeys) {
      variables.push_back(ordering.at(key));
    }
//    std::pair<GaussianFactorGraph::sharedConditional, GaussianFactorGraph> result = graph.eliminate(variables);
    GaussianFactorGraph::EliminationResult result = EliminateQR(graph, marginalizeKeys.size());
result.first->print("Resulting Conditional:\n");
result.second->print("Resulting Linear Factor:\n");
//    graph = result.second;
    graph.replace(0, result.second);

    //  (3) convert the marginal factors into Linearized Factors
    NonlinearFactor::shared_ptr marginalFactor;
    assert(graph.size() <= 1);
    if(graph.size() > 0) {
graph.at(0)->print("Linear Factor After:\n");
      // These factors are all generated from BayesNet conditionals. They should all be Jacobians.
      JacobianFactor::shared_ptr jacobianFactor = boost::dynamic_pointer_cast<JacobianFactor>(graph.at(0));
      assert(jacobianFactor);
      marginalFactor = LinearizedJacobianFactor::shared_ptr(new LinearizedJacobianFactor(jacobianFactor, ordering, theta));
    }
marginalFactor->print("Factor After:\n");
    return marginalFactor;
  }
}

/* ************************************************************************* */

}/// namespace gtsam
