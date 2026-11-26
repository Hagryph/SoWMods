#pragma once
#include "../../shared/ModActions.h"
#include <memory>
#include <string>
#include <vector>
namespace sow::actions {
struct Command {
    std::string id;
    sowmod::actions::Action spec; // descriptors in the mod must have static lifetime
};
struct Result {bool ok; std::string text;};
struct Job;
bool RegisterMod(const std::string& prefix,sowmod::actions::Register fn,std::vector<Command>& added);
std::vector<Command> Commands();
std::shared_ptr<Job> Submit(const Command&,std::vector<std::string>,int choice=-1,std::string search={});
bool Poll(const std::shared_ptr<Job>&,Result&);
void StartPipe();
void DrawForm(const std::string& id);
}
